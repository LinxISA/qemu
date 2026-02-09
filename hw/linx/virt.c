/*
 * QEMU LinxISA "virt" machine
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/guest-random.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/core/boards.h"
#include "system/address-spaces.h"
#include "system/device_tree.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "elf.h"
#include "chardev/char.h"
#include "chardev/char-fe.h"
#include "qemu/qemu-print.h"
#include "system/system.h"

#include "cpu.h"

#include <libfdt.h>

static bool linx_virt_debug_enabled(void)
{
    const char *v = getenv("LINX_VIRT_DEBUG");
    return v && v[0] && strcmp(v, "0") != 0;
}

#if TARGET_LONG_BITS == 32
static const char *linx_elf32_sym_name(const uint8_t *buf, size_t len,
                                       const Elf32_Shdr *strtab_sh,
                                       const Elf32_Sym *sym)
{
    if (!strtab_sh) {
        return "<no-strtab>";
    }
    if ((size_t)strtab_sh->sh_offset + strtab_sh->sh_size > len) {
        return "<bad-strtab>";
    }
    if (sym->st_name >= strtab_sh->sh_size) {
        return "<bad-name>";
    }
    return (const char *)(buf + strtab_sh->sh_offset + sym->st_name);
}
#endif

#if TARGET_LONG_BITS != 32
static const char *linx_elf64_sym_name(const uint8_t *buf, size_t len,
                                       const Elf64_Shdr *strtab_sh,
                                       const Elf64_Sym *sym)
{
    if (!strtab_sh) {
        return "<no-strtab>";
    }
    if ((size_t)strtab_sh->sh_offset + strtab_sh->sh_size > len) {
        return "<bad-strtab>";
    }
    if (sym->st_name >= strtab_sh->sh_size) {
        return "<bad-name>";
    }
    return (const char *)(buf + strtab_sh->sh_offset + sym->st_name);
}
#endif

/* UART base address */
#define LINX_UART_BASE 0x10000000
#define LINX_UART_SIZE 0x100

/* Exit register address */
#define LINX_EXIT_REG 0x10000004

/* Linx virt-uart MMIO register layout (offsets from LINX_UART_BASE). */
#define LINX_UART_DATA_REG 0x0
#define LINX_UART_STATUS_REG 0x4
#define LINX_UART_STATUS_TX_READY 0x1
#define LINX_UART_STATUS_RX_READY 0x2

#define LINX_UART_RX_BUFSZ 256

/*
 * For ET_REL kernel objects, the Linx virt machine uses a split layout:
 * - .text/.rodata/... are placed at LINX_KERNEL_LOAD_BASE (default 0x10000)
 * - zero-initialized data (.bss and common symbols) are placed below it at
 *   LINX_BSS_BASE (default 0x8000)
 *
 * This keeps PCR17 stores/loads to .bss within range for the bring-up toolchain
 * (see R_LINX_PCR17_{LOAD,STORE}).
 */
#define LINX_BSS_BASE 0x00008000

/* ELF relocation types (must match toolchain definitions). */
#define R_LINX_NONE 0
#define R_LINX_B17_PCREL 4
#define R_LINX_HL_BSTART30_PCREL 5
#define R_LINX_PCREL_HI20 15
#define R_LINX_LO12 17
#define R_LINX_PCR17_LOAD 18
#define R_LINX_PCR17_STORE 19
#define R_LINX_HL_PCR29_LOAD 20
#define R_LINX_HL_PCR29_STORE 21
#define R_LINX_64 10
#define R_LINX_32 11

/* Simple UART state */
typedef struct LinxUARTState {
    MemoryRegion mmio;
    QemuMutex lock;
    LinxCPU *cpu;

    CharFrontend chr;

    uint8_t rx_buf[LINX_UART_RX_BUFSZ];
    uint32_t rx_head;
    uint32_t rx_tail;
} LinxUARTState;

static int linx_uart_can_receive(void *opaque)
{
    LinxUARTState *s = opaque;
    int space;

    qemu_mutex_lock(&s->lock);
    if (s->rx_head >= s->rx_tail) {
        space = (LINX_UART_RX_BUFSZ - 1) - (int)(s->rx_head - s->rx_tail);
    } else {
        space = (int)(s->rx_tail - s->rx_head - 1);
    }
    qemu_mutex_unlock(&s->lock);

    return space;
}

static void linx_uart_receive(void *opaque, const uint8_t *buf, int size)
{
    LinxUARTState *s = opaque;
    int i;

    qemu_mutex_lock(&s->lock);
    for (i = 0; i < size; i++) {
        uint32_t next = (s->rx_head + 1) % LINX_UART_RX_BUFSZ;

        if (next == s->rx_tail) {
            break;
        }

        s->rx_buf[s->rx_head] = buf[i];
        s->rx_head = next;
    }
    qemu_mutex_unlock(&s->lock);
}

static uint64_t linx_uart_read(void *opaque, hwaddr addr, unsigned size)
{
    LinxUARTState *s = opaque;
    (void)size;

    if (addr == LINX_UART_DATA_REG) {
        uint8_t c = 0;

        qemu_mutex_lock(&s->lock);
        if (s->rx_head != s->rx_tail) {
            c = s->rx_buf[s->rx_tail];
            s->rx_tail = (s->rx_tail + 1) % LINX_UART_RX_BUFSZ;
        }
        qemu_mutex_unlock(&s->lock);

        qemu_chr_fe_accept_input(&s->chr);
        return c;
    } else if (addr >= LINX_UART_STATUS_REG && addr < LINX_UART_STATUS_REG + 4) {
        uint64_t st = LINX_UART_STATUS_TX_READY;

        qemu_mutex_lock(&s->lock);
        if (s->rx_head != s->rx_tail) {
            st |= LINX_UART_STATUS_RX_READY;
        }
        qemu_mutex_unlock(&s->lock);

        return st;
    }
    return 0;
}

static void linx_uart_write(void *opaque, hwaddr addr, uint64_t value,
                            unsigned size)
{
    LinxUARTState *s = opaque;
    unsigned char c;
    (void)size;

    /* Handle exit register */
    if (addr == LINX_EXIT_REG - LINX_UART_BASE) {
        if (s->cpu) {
            CPULinxState *env = &s->cpu->env;
            fprintf(stderr, "LINX_INSN_COUNT=%" PRIu64 "\n", env->insn_count);
            fflush(stderr);
        }
        if (linx_virt_debug_enabled()) {
            fprintf(stderr, "linx virt: exit mmio write value=0x%" PRIx64 "\n", value);
            fflush(stderr);
        }
        qemu_system_shutdown_request_with_code(SHUTDOWN_CAUSE_GUEST_SHUTDOWN,
                                               (int)(uint32_t)value);
        return;
    }

    if (addr != 0) {
        return;  /* Ignore non-data writes */
    }

    qemu_mutex_lock(&s->lock);
    c = (unsigned char)(value & 0xFF);
    if (linx_virt_debug_enabled()) {
        static int uart_debug_count;
        if (uart_debug_count < 64) {
            fprintf(stderr, "linx virt: uart mmio write value=0x%02x ('%c')\n",
                    (unsigned)c, (c >= 32 && c < 127) ? c : '.');
            fflush(stderr);
            uart_debug_count++;
        }
    }
    if (qemu_chr_fe_backend_connected(&s->chr)) {
        qemu_chr_fe_write_all(&s->chr, &c, 1);
    } else {
        /* Fallback if no chardev backend is available. */
        fputc(c, stdout);
        fflush(stdout);
    }
    qemu_mutex_unlock(&s->lock);
}

static const MemoryRegionOps linx_uart_ops = {
    .read = linx_uart_read,
    .write = linx_uart_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void linx_uart_init(LinxUARTState *s, LinxCPU *cpu)
{
    Chardev *chr;
    Error *err = NULL;

    s->cpu = cpu;
    qemu_mutex_init(&s->lock);
    memory_region_init_io(&s->mmio, NULL, &linx_uart_ops, s,
                          "linx-uart", LINX_UART_SIZE);
    memory_region_add_subregion(get_system_memory(), LINX_UART_BASE,
                                &s->mmio);

    s->rx_head = 0;
    s->rx_tail = 0;

    chr = serial_hd(0);
    if (!chr) {
        chr = qemu_chr_new("linx-uart", "stdio", NULL);
        if (!chr) {
            return;
        }
    }

    if (!qemu_chr_fe_init(&s->chr, chr, &err)) {
        if (err) {
            error_report_err(err);
        }
        return;
    }

    qemu_chr_fe_set_handlers(&s->chr, linx_uart_can_receive, linx_uart_receive,
                             NULL, NULL, s, NULL, true);
    qemu_chr_fe_set_echo(&s->chr, false);
}

#define TYPE_LINX_VIRT_MACHINE MACHINE_TYPE_NAME("virt")
OBJECT_DECLARE_SIMPLE_TYPE(LinxVirtMachineState, LINX_VIRT_MACHINE)

typedef struct LinxVirtMachineState {
    MachineState parent_obj;

    LinxCPU *cpu;

    hwaddr entry;
    hwaddr initial_sp;
    hwaddr exit_trampoline;
    hwaddr fdt_addr;
    
    LinxUARTState uart;
} LinxVirtMachineState;

static hwaddr linx_align_up(hwaddr v, hwaddr align)
{
    if (align <= 1) {
        return v;
    }
    return (v + align - 1) & ~(align - 1);
}

static void *linx_virt_build_fdt(MachineState *machine,
                                 hwaddr mem_size,
                                 hwaddr initrd_base, hwaddr initrd_size,
                                 int *fdt_alloc_size)
{
    void *fdt;
    char *nodename;
    uint8_t rng_seed[32];

    fdt = create_device_tree(fdt_alloc_size);
    if (!fdt) {
        error_report("linx virt: create_device_tree() failed");
        exit(1);
    }

    qemu_fdt_setprop_string(fdt, "/", "compatible", "qemu,linx-virt");
    qemu_fdt_setprop_cell(fdt, "/", "#address-cells", 0x2);
    qemu_fdt_setprop_cell(fdt, "/", "#size-cells", 0x2);

    qemu_fdt_add_subnode(fdt, "/chosen");
    if (machine->kernel_cmdline) {
        qemu_fdt_setprop_string(fdt, "/chosen", "bootargs",
                                machine->kernel_cmdline);
    }
    qemu_fdt_setprop_string(fdt, "/chosen", "stdout-path", "serial0");
    qemu_guest_getrandom_nofail(rng_seed, sizeof(rng_seed));
    qemu_fdt_setprop(fdt, "/chosen", "rng-seed", rng_seed, sizeof(rng_seed));
    if (initrd_size) {
        qemu_fdt_setprop_u64(fdt, "/chosen", "linux,initrd-start",
                             initrd_base);
        qemu_fdt_setprop_u64(fdt, "/chosen", "linux,initrd-end",
                             initrd_base + initrd_size);
    }

    qemu_fdt_add_subnode(fdt, "/memory@0");
    qemu_fdt_setprop_string(fdt, "/memory@0", "device_type", "memory");
    /*
     * Keep the DT memory description consistent with the Linx `virt` physical
     * memory map.
     *
     * The `virt` machine places the UART/exit MMIO window at LINX_UART_BASE.
     * When RAM is large enough to cover that address (e.g. -m 512M), the MMIO
     * region overlaps the RAM window. Split the DT "reg" ranges to exclude the
     * MMIO hole so Linux does not allocate normal pages from it.
     */
    if (mem_size <= (hwaddr)LINX_UART_BASE) {
        qemu_fdt_setprop_cells(fdt, "/memory@0", "reg",
                               0x0, 0x0,
                               (uint32_t)(mem_size >> 32), (uint32_t)mem_size);
    } else {
        const hwaddr mem0_base = 0;
        const hwaddr mem0_size = (hwaddr)LINX_UART_BASE;
        const hwaddr mem1_base = (hwaddr)LINX_UART_BASE + (hwaddr)LINX_UART_SIZE;
        const hwaddr mem1_size = (mem_size > mem1_base) ? (mem_size - mem1_base) : 0;

        if (mem1_size) {
            qemu_fdt_setprop_cells(fdt, "/memory@0", "reg",
                                   (uint32_t)(mem0_base >> 32), (uint32_t)mem0_base,
                                   (uint32_t)(mem0_size >> 32), (uint32_t)mem0_size,
                                   (uint32_t)(mem1_base >> 32), (uint32_t)mem1_base,
                                   (uint32_t)(mem1_size >> 32), (uint32_t)mem1_size);
        } else {
            qemu_fdt_setprop_cells(fdt, "/memory@0", "reg",
                                   (uint32_t)(mem0_base >> 32), (uint32_t)mem0_base,
                                   (uint32_t)(mem0_size >> 32), (uint32_t)mem0_size);
        }
    }

    qemu_fdt_add_subnode(fdt, "/cpus");
    qemu_fdt_setprop_cell(fdt, "/cpus", "#address-cells", 0x1);
    qemu_fdt_setprop_cell(fdt, "/cpus", "#size-cells", 0x0);
    qemu_fdt_add_subnode(fdt, "/cpus/cpu@0");
    qemu_fdt_setprop_string(fdt, "/cpus/cpu@0", "device_type", "cpu");
    qemu_fdt_setprop_string(fdt, "/cpus/cpu@0", "compatible", "linx,linxisa");
    qemu_fdt_setprop_cell(fdt, "/cpus/cpu@0", "reg", 0x0);

    qemu_fdt_add_subnode(fdt, "/soc");
    qemu_fdt_setprop(fdt, "/soc", "ranges", NULL, 0);
    qemu_fdt_setprop_string(fdt, "/soc", "compatible", "simple-bus");
    qemu_fdt_setprop_cell(fdt, "/soc", "#address-cells", 0x2);
    qemu_fdt_setprop_cell(fdt, "/soc", "#size-cells", 0x2);

    nodename = g_strdup_printf("/soc/serial@%" HWADDR_PRIx,
                               (hwaddr)LINX_UART_BASE);
    qemu_fdt_add_subnode(fdt, nodename);
    qemu_fdt_setprop_string(fdt, nodename, "compatible", "linx,virt-uart");
    qemu_fdt_setprop_cells(fdt, nodename, "reg",
                           0x0, (uint32_t)LINX_UART_BASE,
                           0x0, (uint32_t)LINX_UART_SIZE);
    qemu_fdt_setprop_cell(fdt, nodename, "current-speed", 115200);

    qemu_fdt_add_subnode(fdt, "/aliases");
    qemu_fdt_setprop_string(fdt, "/aliases", "serial0", nodename);

    g_free(nodename);
    return fdt;
}

static void *linx_virt_get_fdt(MachineState *machine,
                               hwaddr mem_size,
                               hwaddr initrd_base, hwaddr initrd_size,
                               int *fdt_alloc_size)
{
    void *fdt;
    uint8_t rng_seed[32];

    if (machine->dtb) {
        fdt = load_device_tree(machine->dtb, fdt_alloc_size);
        if (!fdt) {
            error_report("linx virt: couldn't open dtb file %s", machine->dtb);
            exit(1);
        }
        if (machine->kernel_cmdline) {
            qemu_fdt_setprop_string(fdt, "/chosen", "bootargs",
                                    machine->kernel_cmdline);
        }
        qemu_fdt_setprop_string(fdt, "/chosen", "stdout-path", "serial0");
        qemu_guest_getrandom_nofail(rng_seed, sizeof(rng_seed));
        qemu_fdt_setprop(fdt, "/chosen", "rng-seed", rng_seed, sizeof(rng_seed));
        if (initrd_size) {
            qemu_fdt_setprop_u64(fdt, "/chosen", "linux,initrd-start",
                                 initrd_base);
            qemu_fdt_setprop_u64(fdt, "/chosen", "linux,initrd-end",
                                 initrd_base + initrd_size);
        }
        return fdt;
    }

    return linx_virt_build_fdt(machine, mem_size,
                               initrd_base, initrd_size,
                               fdt_alloc_size);
}

static uint64_t linx_ld48_le_p(const uint8_t *p);

/* Check if an address points to a block-start instruction.
 *
 * In addition to explicit BSTART encodings, the bring-up toolchain emits
 * standalone frame macro blocks (FENTRY/FEXIT/FRET.*) that should also be
 * treated as valid code targets.
 */
static bool linx_is_bstart(const uint8_t *ram, size_t ram_size, hwaddr addr)
{
    uint16_t hw;
    uint32_t insn;
    
    if (addr + 2 > ram_size) {
        return false;
    }
    
    hw = lduw_le_p(ram + addr);
    
    /* C.BSTART.STD: mask=0xc7ff, match=0x0000, BrType field */
    if ((hw & 0xc7ff) == 0x0000) {
        return true;
    }
    
    /* C.BSTART DIRECT/COND: check opcode bits */
    if ((hw & 0x000f) == 0x0002 || (hw & 0x000f) == 0x0004) {
        return true;
    }
    
    /* Check for 32-bit BSTART instructions */
    if (addr + 4 > ram_size) {
        return false;
    }
    
    insn = ldl_le_p(ram + addr);
    
    /* BSTART.STD FALL: 0x1001 */
    if ((insn & 0x7fff) == 0x1001) {
        return true;
    }
    
    /* BSTART.STD DIRECT: 0x2001 */
    if ((insn & 0x7fff) == 0x2001) {
        return true;
    }
    
    /* BSTART.STD COND: 0x3001 */
    if ((insn & 0x7fff) == 0x3001) {
        return true;
    }
    
    /* BSTART.STD CALL: 0x4001 */
    if ((insn & 0x7fff) == 0x4001) {
        return true;
    }

    /* Frame macro instructions (FENTRY/FEXIT/FRET.*): opcode 0x41 in bits [6:0]. */
    if ((insn & 0x7f) == 0x41) {
        return true;
    }

    /* 48-bit HL.BSTART.* block markers (prefix + 32-bit main instruction). */
    if (addr + 6 <= ram_size) {
        uint64_t insn48 = linx_ld48_le_p(ram + addr);

        /*
         * HL.BSTART.{STD,SYS,FP} share these fixed bits:
         * - bits[3:0]  == 0b1110 (HL trailer)
         * - bits[22:16] == 0b0000001 (bit16=1, bits22:17=0)
         * brtype lives in bits[30:28] and group in bits[27:23].
         */
        if ((insn48 & 0x00007f000fULL) == 0x00000001000eULL) {
            unsigned group = (insn48 >> 23) & 0x1f;
            unsigned brtype = (insn48 >> 28) & 0x7;
            if (group <= 2 && brtype >= 1 && brtype <= 4) {
                return true;
            }
        }

        /* 48-bit HL.BSTART.CALL (32-bit main + 16-bit extension). */
        if ((insn48 & 0xf83f0000007fULL) == 0x501600000011ULL) {
            return true;
        }
    }
    
    return false;
}

/* Find the nearest BSTART instruction backward from addr */
static hwaddr linx_find_bstart_backward(const uint8_t *ram, size_t ram_size,
                                        hwaddr addr, hwaddr search_limit)
{
    hwaddr cur = addr;
    
    /* Search backward, but don't go before search_limit */
    while (cur >= search_limit && cur + 2 <= ram_size) {
        if (linx_is_bstart(ram, ram_size, cur)) {
            return cur;
        }
        /* Instructions are at least 2 bytes aligned */
        if (cur < 2) {
            break;
        }
        cur -= 2;
    }
    
    return addr; /* Return original if not found */
}

/* Ensure an address points to a BSTART instruction, adjusting if necessary */
static hwaddr linx_ensure_bstart(const uint8_t *ram, size_t ram_size,
                                  hwaddr addr, hwaddr section_start)
{
    if (linx_is_bstart(ram, ram_size, addr)) {
        return addr;
    }
    
    /* Try to find BSTART backward (within reasonable distance) */
    hwaddr found = linx_find_bstart_backward(ram, ram_size, addr,
                                             section_start);
    if (found != addr && linx_is_bstart(ram, ram_size, found)) {
        return found;
    }
    
    /* If still not found, return original (will be caught as error) */
    return addr;
}

static bool linx_patch_bstart_call_pcrel(uint8_t *ram, size_t ram_size,
                                         hwaddr patch_addr, hwaddr target_addr,
                                         int64_t addend, Error **errp)
{
    uint16_t hw;
    uint32_t insn;
    int64_t delta;
    int64_t simm17;
    uint32_t imm_bits;

    if (patch_addr + 4 > ram_size) {
        error_setg(errp, "relocation patch out of RAM bounds @ 0x%" HWADDR_PRIx,
                   patch_addr);
        return false;
    }

    hw = lduw_le_p(ram + patch_addr);
    if ((hw & 1) == 0) {
        error_setg(errp,
                   "unsupported relocation: expected 32-bit instruction @ 0x%" HWADDR_PRIx,
                   patch_addr);
        return false;
    }

    insn = ldl_le_p(ram + patch_addr);
    if ((insn & 0x7fff) != 0x4001) {
        error_setg(errp,
                   "unsupported relocation: not a BSTART.CALL (insn=0x%08x) @ 0x%" HWADDR_PRIx,
                   insn, patch_addr);
        return false;
    }

    delta = (int64_t)(target_addr + addend) - (int64_t)patch_addr;
    if (delta & 1) {
        error_setg(errp,
                   "unaligned call target: patch @ 0x%" HWADDR_PRIx " -> 0x%" HWADDR_PRIx,
                   patch_addr, target_addr);
        return false;
    }

    simm17 = delta >> 1;
    if (simm17 < -(1 << 16) || simm17 >= (1 << 16)) {
        error_setg(errp,
                   "call target out of range: patch @ 0x%" HWADDR_PRIx " -> 0x%" HWADDR_PRIx,
                   patch_addr, target_addr);
        return false;
    }

    imm_bits = (uint32_t)(simm17 & 0x1ffff);
    insn = (insn & 0x7fff) | (imm_bits << 15);
    stl_le_p(ram + patch_addr, insn);
    return true;
}

/* Patch an ADDTPC instruction with a PC-relative page offset.
 *
 * ADDTPC encodes a signed imm20 in bits [31:12] which is scaled by 4KiB
 * (imm20 << 12) and added to the current PC page base.
 *
 * Relocation value:
 *   (S + A) page - (P) page
 * where page(x) = x & ~0xFFF.
 */
static bool linx_patch_addtpc_pcrel(uint8_t *ram, size_t ram_size,
                                    hwaddr patch_addr, hwaddr target_addr,
                                    int64_t addend, Error **errp)
{
    uint32_t insn;
    int64_t delta;
    int64_t simm20;
    uint32_t imm_bits;

    if (patch_addr + 4 > ram_size) {
        error_setg(errp, "ADDTPC relocation patch out of RAM bounds @ 0x%" HWADDR_PRIx,
                   patch_addr);
        return false;
    }

    insn = ldl_le_p(ram + patch_addr);
    
    /* ADDTPC has opcode 0x07 in bits [6:0] */
    if ((insn & 0x7f) != 0x07) {
        error_setg(errp,
                   "expected ADDTPC instruction (insn=0x%08x) @ 0x%" HWADDR_PRIx,
                   insn, patch_addr);
        return false;
    }

    /* Calculate page delta in bytes: page(S+A) - page(P) */
    hwaddr target_page = (target_addr + addend) & ~(hwaddr)0xfff;
    hwaddr patch_page = patch_addr & ~(hwaddr)0xfff;
    delta = (int64_t)target_page - (int64_t)patch_page;

    simm20 = delta >> 12;
    if (simm20 < -(1LL << 19) || simm20 >= (1LL << 19)) {
        error_setg(errp,
                   "ADDTPC page delta out of range: patch @ 0x%" HWADDR_PRIx " -> 0x%" HWADDR_PRIx " (delta=%" PRId64 ")",
                   patch_addr, target_addr, delta);
        return false;
    }

    /* Encode imm20 in bits [31:12] */
    imm_bits = (uint32_t)(simm20 & 0xfffff);
    insn = (insn & 0xfff) | (imm_bits << 12);
    stl_le_p(ram + patch_addr, insn);
    return true;
}

/* Patch an ADDI/ADDIW uimm12 immediate with the low 12 bits of S+A.
 * imm12 goes in bits [31:20].
 */
static bool linx_patch_lo12_uimm12(uint8_t *ram, size_t ram_size,
                                   hwaddr patch_addr, hwaddr target_addr,
                                   int64_t addend, Error **errp)
{
    uint32_t insn;
    uint32_t imm12;

    if (patch_addr + 4 > ram_size) {
        error_setg(errp, "LO12 relocation patch out of RAM bounds @ 0x%" HWADDR_PRIx,
                   patch_addr);
        return false;
    }

    insn = ldl_le_p(ram + patch_addr);

    /* ADDI opcode is 0x15, ADDIW opcode is 0x35 (bits [6:0]). */
    switch (insn & 0x7f) {
    case 0x15:
    case 0x35:
        break;
    default:
        error_setg(errp,
                   "expected ADDI/ADDIW instruction for LO12 (insn=0x%08x) @ 0x%" HWADDR_PRIx,
                   insn, patch_addr);
        return false;
    }

    imm12 = (uint32_t)((target_addr + addend) & 0xFFFu);
    insn = (insn & 0x000FFFFFu) | (imm12 << 20);
    stl_le_p(ram + patch_addr, insn);
    return true;
}

/* Patch a PC-relative load instruction (LD.PCR, LW.PCR, etc.)
 * These have opcode 0x39 in bits [6:0], with size in bits [14:12]
 * simm17 (halfword-scaled) goes into bits [31:15]
 */
static bool linx_patch_ld_pcr(uint8_t *ram, size_t ram_size,
                              hwaddr patch_addr, hwaddr target_addr,
                              int64_t addend, Error **errp)
{
    uint32_t insn;
    int64_t delta;
    int64_t simm17;
    uint32_t imm_bits;

    if (patch_addr + 4 > ram_size) {
        error_setg(errp, "LD.PCR relocation out of bounds @ 0x%" HWADDR_PRIx,
                   patch_addr);
        return false;
    }

    insn = ldl_le_p(ram + patch_addr);
    
    delta = (int64_t)(target_addr + addend) - (int64_t)patch_addr;
    if (delta & 1) {
        error_setg(errp,
                   "unaligned LD.PCR target @ 0x%" HWADDR_PRIx,
                   patch_addr);
        return false;
    }

    simm17 = delta >> 1;
    if (simm17 < -(1 << 16) || simm17 >= (1 << 16)) {
        error_setg(errp,
                   "LD.PCR target out of range @ 0x%" HWADDR_PRIx " (delta=%" PRId64 ")",
                   patch_addr, delta);
        return false;
    }

    imm_bits = (uint32_t)(simm17 & 0x1ffff);
    insn = (insn & 0x7fff) | (imm_bits << 15);
    stl_le_p(ram + patch_addr, insn);
    return true;
}

/* Patch a PC-relative store instruction (SD.PCR, SW.PCR, etc.)
 *
 * These have opcode 0x69 in bits [6:0], with size in bits [14:12] and SrcL in
 * bits [19:15].
 *
 * For the 32-bit ST.PCR family, the signed immediate is encoded as a 17-bit
 * value split across:
 *   - imm[11:0]  -> bits [31:20]
 *   - imm[16:12] -> bits [11:7]
 *
 * Address = TPC + simm17 (byte offset).
 */
static bool linx_patch_st_pcr(uint8_t *ram, size_t ram_size,
                              hwaddr patch_addr, hwaddr target_addr,
                              int64_t addend, Error **errp)
{
    uint32_t insn;
    int64_t delta;
    int64_t simm17;
    uint32_t imm, imm_lo12, imm_hi5;

    if (patch_addr + 4 > ram_size) {
        error_setg(errp, "ST.PCR relocation out of bounds @ 0x%" HWADDR_PRIx,
                   patch_addr);
        return false;
    }

    insn = ldl_le_p(ram + patch_addr);
    
    /* PC-relative byte offset */
    delta = (int64_t)(target_addr + addend) - (int64_t)patch_addr;

    /* Check range - need to fit in signed 17-bit split encoding */
    simm17 = delta;
    if (simm17 < -(1 << 16) || simm17 >= (1 << 16)) {
        error_setg(errp,
                   "ST.PCR target out of range @ 0x%" HWADDR_PRIx " (delta=%" PRId64 ")",
                   patch_addr, delta);
        return false;
    }

    imm = (uint32_t)(simm17 & 0x1ffffu);
    imm_lo12 = imm & 0xfffu;
    imm_hi5 = (imm >> 12) & 0x1fu;

    /* Preserve opcode + funct3 + SrcL, patch split immediate. */
    insn = (insn & 0x000ff07fu) | (imm_lo12 << 20) | (imm_hi5 << 7);
    stl_le_p(ram + patch_addr, insn);
    return true;
}

static uint64_t linx_ld48_le_p(const uint8_t *p)
{
    /* 48-bit little-endian load */
    return (uint64_t)p[0] |
           ((uint64_t)p[1] << 8) |
           ((uint64_t)p[2] << 16) |
           ((uint64_t)p[3] << 24) |
           ((uint64_t)p[4] << 32) |
           ((uint64_t)p[5] << 40);
}

static void linx_st48_le_p(uint8_t *p, uint64_t v)
{
    /* 48-bit little-endian store */
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
    p[4] = (uint8_t)((v >> 32) & 0xff);
    p[5] = (uint8_t)((v >> 40) & 0xff);
}

/* Patch a 48-bit HL.*.PCR load instruction (HL.LW.PCR, HL.LD.PCR, etc.)
 *
 * HL.*.PCR loads use a signed 29-bit byte offset split across:
 *   - simm[16:0]  -> insn[47:31]
 *   - simm[28:17] -> insn[15:4]
 *
 * Address = TPC + simm29 (byte offset).
 */
static bool linx_patch_hl_ld_pcr(uint8_t *ram, size_t ram_size,
                                 hwaddr patch_addr, hwaddr target_addr,
                                 int64_t addend, Error **errp)
{
    uint64_t insn48;
    int64_t delta;
    int64_t simm29;
    uint64_t imm, imm_lo17, imm_hi12;

    if (patch_addr + 6 > ram_size) {
        error_setg(errp, "HL.LD.PCR relocation out of bounds @ 0x%" HWADDR_PRIx,
                   patch_addr);
        return false;
    }

    insn48 = linx_ld48_le_p(ram + patch_addr);
    delta = (int64_t)(target_addr + addend) - (int64_t)patch_addr;

    simm29 = delta;
    if (simm29 < -(1LL << 28) || simm29 >= (1LL << 28)) {
        error_setg(errp,
                   "HL.LD.PCR target out of range @ 0x%" HWADDR_PRIx " (delta=%" PRId64 ")",
                   patch_addr, delta);
        return false;
    }

    imm = (uint64_t)(simm29 & 0x1fffffffULL);
    imm_lo17 = imm & 0x1ffffu;
    imm_hi12 = (imm >> 17) & 0xfffu;

    insn48 = (insn48 & ~(((uint64_t)0x1ffffu << 31) | ((uint64_t)0xfffu << 4))) |
             (imm_lo17 << 31) | (imm_hi12 << 4);
    linx_st48_le_p(ram + patch_addr, insn48);
    return true;
}

/* Patch a 48-bit HL.*.PCR store instruction (HL.SW.PCR, HL.SD.PCR, etc.)
 *
 * HL.*.PCR stores use a signed 29-bit byte offset split across:
 *   - simm[11:0]  -> insn[47:36]
 *   - simm[16:12] -> insn[27:23]
 *   - simm[28:17] -> insn[15:4]
 *
 * Address = TPC + simm29 (byte offset).
 */
static bool linx_patch_hl_st_pcr(uint8_t *ram, size_t ram_size,
                                 hwaddr patch_addr, hwaddr target_addr,
                                 int64_t addend, Error **errp)
{
    uint64_t insn48;
    int64_t delta;
    int64_t simm29;
    uint64_t imm, imm_lo12, imm_mid5, imm_hi12;

    if (patch_addr + 6 > ram_size) {
        error_setg(errp, "HL.ST.PCR relocation out of bounds @ 0x%" HWADDR_PRIx,
                   patch_addr);
        return false;
    }

    insn48 = linx_ld48_le_p(ram + patch_addr);
    delta = (int64_t)(target_addr + addend) - (int64_t)patch_addr;

    simm29 = delta;
    if (simm29 < -(1LL << 28) || simm29 >= (1LL << 28)) {
        error_setg(errp,
                   "HL.ST.PCR target out of range @ 0x%" HWADDR_PRIx " (delta=%" PRId64 ")",
                   patch_addr, delta);
        return false;
    }

    imm = (uint64_t)(simm29 & 0x1fffffffULL);
    imm_lo12 = imm & 0xfffu;
    imm_mid5 = (imm >> 12) & 0x1fu;
    imm_hi12 = (imm >> 17) & 0xfffu;

    insn48 = (insn48 & ~(((uint64_t)0xfffu << 36) | ((uint64_t)0x1fu << 23) |
                         ((uint64_t)0xfffu << 4))) |
             (imm_lo12 << 36) | (imm_mid5 << 23) | (imm_hi12 << 4);
    linx_st48_le_p(ram + patch_addr, insn48);
    return true;
}

/* Patch a 48-bit HL.BSTART.{STD,SYS,FP} PC-relative control-flow marker.
 *
 * The encoded immediate is a signed 29-bit halfword offset:
 *   BNextOffset = simm29 << 1
 *
 * The immediate is split across:
 *   - simm[16:0]  -> insn[47:31]
 *   - simm[28:17] -> insn[15:4]
 */
static bool linx_patch_hl_bstart_pcrel(uint8_t *ram, size_t ram_size,
                                       hwaddr patch_addr, hwaddr target_addr,
                                       int64_t addend, Error **errp)
{
    uint64_t insn48;
    int64_t delta;
    int64_t simm29;
    uint64_t imm, imm_lo17, imm_hi12;

    if (patch_addr + 6 > ram_size) {
        error_setg(errp, "HL.BSTART relocation out of bounds @ 0x%" HWADDR_PRIx,
                   patch_addr);
        return false;
    }

    insn48 = linx_ld48_le_p(ram + patch_addr);
    if ((insn48 & 0x00007f000fULL) != 0x00000001000eULL) {
        error_setg(errp,
                   "expected HL.BSTART.* instruction for HL.BSTART relocation (insn48=0x%012" PRIx64 ") @ 0x%" HWADDR_PRIx,
                   insn48, patch_addr);
        return false;
    }

    delta = (int64_t)(target_addr + addend) - (int64_t)patch_addr;
    if (delta & 1) {
        error_setg(errp,
                   "unaligned HL.BSTART target: patch @ 0x%" HWADDR_PRIx " -> 0x%" HWADDR_PRIx,
                   patch_addr, target_addr);
        return false;
    }

    simm29 = delta >> 1;
    if (simm29 < -(1LL << 28) || simm29 >= (1LL << 28)) {
        error_setg(errp,
                   "HL.BSTART target out of range @ 0x%" HWADDR_PRIx " (delta=%" PRId64 ")",
                   patch_addr, delta);
        return false;
    }

    imm = (uint64_t)(simm29 & 0x1fffffffULL);
    imm_lo17 = imm & 0x1ffffu;
    imm_hi12 = (imm >> 17) & 0xfffu;

    insn48 = (insn48 & ~(((uint64_t)0x1ffffu << 31) | ((uint64_t)0xfffu << 4))) |
             (imm_lo17 << 31) | (imm_hi12 << 4);
    linx_st48_le_p(ram + patch_addr, insn48);
    return true;
}

/* Generic relocation handler that dispatches based on instruction type */
static bool linx_patch_reloc(uint8_t *ram, size_t ram_size,
                             hwaddr patch_addr, hwaddr target_addr,
                             int64_t addend, bool target_is_bstart,
                             Error **errp)
{
    uint16_t hw;
    uint32_t insn;
    uint8_t opcode;

    if (patch_addr + 4 > ram_size) {
        error_setg(errp, "relocation patch out of RAM bounds @ 0x%" HWADDR_PRIx,
                   patch_addr);
        return false;
    }

    hw = lduw_le_p(ram + patch_addr);
    if ((hw & 1) == 0) {
        error_setg(errp,
                   "unsupported relocation: expected 32-bit instruction @ 0x%" HWADDR_PRIx,
                   patch_addr);
        return false;
    }

    insn = ldl_le_p(ram + patch_addr);
    opcode = insn & 0x7f;

    /* Check instruction type and dispatch */
    if (opcode == 0x07) {
        /* ADDTPC instruction - PC-relative data address */
        return linx_patch_addtpc_pcrel(ram, ram_size, patch_addr, target_addr,
                                       addend, errp);
    } else if (opcode == 0x15 || opcode == 0x35) {
        /* ADDI/ADDIW low 12-bit absolute immediate */
        return linx_patch_lo12_uimm12(ram, ram_size, patch_addr, target_addr,
                                      addend, errp);
    } else if (opcode == 0x39) {
        /* LD.PCR family - PC-relative loads */
        return linx_patch_ld_pcr(ram, ram_size, patch_addr, target_addr,
                                 addend, errp);
    } else if (opcode == 0x69) {
        /* ST.PCR family - PC-relative stores */
        return linx_patch_st_pcr(ram, ram_size, patch_addr, target_addr,
                                 addend, errp);
    } else if ((insn & 0x7fff) == 0x4001) {
        /* BSTART.CALL instruction */
        return linx_patch_bstart_call_pcrel(ram, ram_size, patch_addr, target_addr,
                                            addend, errp);
    } else {
        error_setg(errp,
                   "unsupported relocation instruction (insn=0x%08x, opcode=0x%02x) @ 0x%" HWADDR_PRIx,
                   insn, opcode, patch_addr);
        return false;
    }
}

#if TARGET_LONG_BITS == 32
static bool linx_load_elf32_rel(const uint8_t *buf, size_t len,
                                uint8_t *ram, size_t ram_size,
                                hwaddr load_base,
                                hwaddr *entry, hwaddr *image_end,
                                Error **errp)
{
    const Elf32_Ehdr *eh;
    const Elf32_Shdr *shdrs;
    const Elf32_Shdr *shstr_sh;
    hwaddr *sec_addr;
    hwaddr cur;
    hwaddr end = load_base;

    const Elf32_Shdr *symtab_sh = NULL;
    const Elf32_Shdr *strtab_sh = NULL;
    const Elf32_Sym *syms = NULL;
    size_t nsyms = 0;
    hwaddr *sym_addr = NULL;
    bool found_start = false;

    size_t i;

    if (len < sizeof(*eh)) {
        error_setg(errp, "file too small for ELF header");
        return false;
    }
    eh = (const Elf32_Ehdr *)buf;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0) {
        error_setg(errp, "not an ELF file");
        return false;
    }
    if (eh->e_ident[EI_CLASS] != ELFCLASS32) {
        error_setg(errp, "expected ELF32 object");
        return false;
    }
    if (eh->e_ident[EI_DATA] != ELFDATA2LSB) {
        error_setg(errp, "unsupported ELF endianness");
        return false;
    }
    if (eh->e_type != ET_REL) {
        error_setg(errp, "expected ET_REL object");
        return false;
    }

    if (eh->e_shoff == 0 || eh->e_shentsize != sizeof(Elf32_Shdr) ||
        eh->e_shnum == 0) {
        error_setg(errp, "invalid section header table");
        return false;
    }
    if ((size_t)eh->e_shoff + (size_t)eh->e_shentsize * eh->e_shnum > len) {
        error_setg(errp, "section header table out of bounds");
        return false;
    }

    shdrs = (const Elf32_Shdr *)(buf + eh->e_shoff);
    if (eh->e_shstrndx == SHN_UNDEF || eh->e_shstrndx >= eh->e_shnum) {
        error_setg(errp, "invalid shstrndx");
        return false;
    }
    shstr_sh = &shdrs[eh->e_shstrndx];
    if ((size_t)shstr_sh->sh_offset + shstr_sh->sh_size > len) {
        error_setg(errp, "shstrtab out of bounds");
        return false;
    }

    sec_addr = g_new0(hwaddr, eh->e_shnum);
    cur = load_base;
    hwaddr bss_cur = LINX_BSS_BASE;
    hwaddr bss_end = bss_cur;

    for (i = 0; i < eh->e_shnum; i++) {
        const Elf32_Shdr *sh = &shdrs[i];
        hwaddr align;
        hwaddr *cursor = &cur;

        if (!(sh->sh_flags & SHF_ALLOC) || sh->sh_size == 0) {
            continue;
        }
        align = sh->sh_addralign ? sh->sh_addralign : 1;
        if ((align & (align - 1)) != 0) {
            error_setg(errp, "invalid section alignment for #%zu", i);
            goto fail;
        }

        /*
         * Place zero-initialized sections (SHT_NOBITS: .bss/.sbss/...) in the
         * low BSS window to keep PC-relative data relocations in range.
         *
         * If the BSS window would overlap the kernel load base, fall back to
         * placing the section after the loaded image.
         */
        if (sh->sh_type == SHT_NOBITS) {
            bss_cur = linx_align_up(bss_cur, align);
            if (bss_cur + sh->sh_size <= load_base) {
                cursor = &bss_cur;
            }
        }

        *cursor = linx_align_up(*cursor, align);
        if ((size_t)*cursor + sh->sh_size > ram_size) {
            error_setg(errp, "section #%zu does not fit in RAM", i);
            goto fail;
        }

        sec_addr[i] = *cursor;
        if (sh->sh_type == SHT_NOBITS) {
            memset(ram + *cursor, 0, sh->sh_size);
        } else {
            if ((size_t)sh->sh_offset + sh->sh_size > len) {
                error_setg(errp, "section #%zu data out of bounds", i);
                goto fail;
            }
            memcpy(ram + *cursor, buf + sh->sh_offset, sh->sh_size);
        }

        *cursor += sh->sh_size;
        if (cursor == &bss_cur) {
            if (bss_cur > bss_end) {
                bss_end = bss_cur;
            }
        } else {
            if (cur > end) {
                end = cur;
            }
        }
    }
    if (bss_end > end) {
        end = bss_end;
    }

    for (i = 0; i < eh->e_shnum; i++) {
        const Elf32_Shdr *sh = &shdrs[i];
        if (sh->sh_type == SHT_SYMTAB) {
            symtab_sh = sh;
            if (sh->sh_link < eh->e_shnum) {
                strtab_sh = &shdrs[sh->sh_link];
            }
            break;
        }
    }
    if (!symtab_sh || !strtab_sh || strtab_sh->sh_type != SHT_STRTAB) {
        error_setg(errp, "missing .symtab/.strtab");
        goto fail;
    }
    if ((size_t)symtab_sh->sh_offset + symtab_sh->sh_size > len ||
        (size_t)strtab_sh->sh_offset + strtab_sh->sh_size > len) {
        error_setg(errp, "symtab/strtab out of bounds");
        goto fail;
    }

    syms = (const Elf32_Sym *)(buf + symtab_sh->sh_offset);
    nsyms = symtab_sh->sh_size / sizeof(Elf32_Sym);
    sym_addr = g_new0(hwaddr, nsyms);

    /* Allocate SHN_COMMON symbols in the BSS window when possible. */
    for (i = 0; i < nsyms; i++) {
        const Elf32_Sym *sym = &syms[i];
        hwaddr align;

        if (sym->st_shndx != SHN_COMMON || sym->st_size == 0) {
            continue;
        }

        align = sym->st_value ? sym->st_value : 1;
        if ((align & (align - 1)) != 0) {
            error_setg(errp, "invalid common symbol alignment for %s",
                       linx_elf32_sym_name(buf, len, strtab_sh, sym));
            goto fail;
        }

        bss_end = linx_align_up(bss_end, align);
        if (bss_end + sym->st_size > load_base) {
            /* Fall back to placing after the loaded image. */
            end = linx_align_up(end, align);
            bss_end = end;
        }

        if ((size_t)bss_end + sym->st_size > ram_size) {
            error_setg(errp, "common symbol %s does not fit in RAM",
                       linx_elf32_sym_name(buf, len, strtab_sh, sym));
            goto fail;
        }

        sym_addr[i] = bss_end;
        memset(ram + bss_end, 0, sym->st_size);
        bss_end += sym->st_size;
        if (bss_end > end) {
            end = bss_end;
        }
    }

    for (i = 0; i < nsyms; i++) {
        const Elf32_Sym *sym = &syms[i];
        if (sym->st_shndx == SHN_UNDEF) {
            continue;
        }
        if (sym->st_shndx == SHN_ABS) {
            sym_addr[i] = sym->st_value;
            continue;
        }
        if (sym->st_shndx < eh->e_shnum) {
            sym_addr[i] = sec_addr[sym->st_shndx] + sym->st_value;
        }
    }

    *entry = 0;
    for (i = 0; i < nsyms; i++) {
        const Elf32_Sym *sym = &syms[i];
        const char *name;

        if (sym->st_name >= strtab_sh->sh_size) {
            continue;
        }
        name = (const char *)(buf + strtab_sh->sh_offset + sym->st_name);
        if (!strcmp(name, "_start")) {
            hwaddr entry_addr = sym_addr[i];
            /* Find the section containing this symbol */
            hwaddr section_start = load_base;
            if (sym->st_shndx < eh->e_shnum) {
                section_start = sec_addr[sym->st_shndx];
            }
            /* Ensure entry point points to a BSTART instruction */
            *entry = linx_ensure_bstart(ram, ram_size, entry_addr, section_start);
            if (*entry != entry_addr && !linx_is_bstart(ram, ram_size, *entry)) {
                error_setg(errp, "entry symbol _start does not point to BSTART instruction");
                goto fail;
            }
            found_start = true;
            break;
        }
    }
    if (!found_start) {
        error_setg(errp, "entry symbol _start not found");
        goto fail;
    }

    for (i = 0; i < eh->e_shnum; i++) {
        const Elf32_Shdr *relsh = &shdrs[i];
        const Elf32_Rela *rela;
        size_t nrela;
        size_t j;
        hwaddr base;

        if (relsh->sh_type != SHT_RELA || relsh->sh_info >= eh->e_shnum) {
            continue;
        }
        if ((size_t)relsh->sh_offset + relsh->sh_size > len) {
            error_setg(errp, "relocation section out of bounds");
            goto fail;
        }

        base = sec_addr[relsh->sh_info];
        rela = (const Elf32_Rela *)(buf + relsh->sh_offset);
        nrela = relsh->sh_size / sizeof(Elf32_Rela);

        for (j = 0; j < nrela; j++) {
            unsigned symidx = ELF32_R_SYM(rela[j].r_info);
            unsigned rtype = ELF32_R_TYPE(rela[j].r_info);
            hwaddr target;
            hwaddr patch_addr;
            uint32_t patch_insn;
            const Elf32_Sym *target_sym = NULL;
            bool target_is_code;

            if (symidx >= nsyms) {
                error_setg(errp, "invalid relocation symbol index %u (nsyms=%zu)",
                           symidx, nsyms);
                goto fail;
            }
            target_sym = &syms[symidx];
            if (target_sym->st_shndx == SHN_UNDEF) {
                error_setg(errp, "undefined symbol %s (index %u) in relocation",
                           linx_elf32_sym_name(buf, len, strtab_sh, target_sym), symidx);
                goto fail;
            }
            if (target_sym->st_shndx < eh->e_shnum && sec_addr[target_sym->st_shndx] == 0) {
                error_setg(errp, "symbol %s refers to non-alloc section #%u",
                           linx_elf32_sym_name(buf, len, strtab_sh, target_sym),
                           target_sym->st_shndx);
                goto fail;
            }

            target = sym_addr[symidx];
            patch_addr = base + rela[j].r_offset;

            if (rtype == R_LINX_32) {
                if (patch_addr + 4 > ram_size) {
                    error_setg(errp, "relocation patch out of RAM bounds @ 0x%" HWADDR_PRIx,
                               patch_addr);
                    goto fail;
                }
                stl_le_p(ram + patch_addr, (uint32_t)(target + (int64_t)rela[j].r_addend));
                continue;
            }

            if (rtype == R_LINX_HL_PCR29_LOAD) {
                if (!linx_patch_hl_ld_pcr(ram, ram_size, patch_addr, target,
                                          (int64_t)rela[j].r_addend, errp)) {
                    goto fail;
                }
                continue;
            } else if (rtype == R_LINX_HL_PCR29_STORE) {
                if (!linx_patch_hl_st_pcr(ram, ram_size, patch_addr, target,
                                          (int64_t)rela[j].r_addend, errp)) {
                    goto fail;
                }
                continue;
            } else if (rtype == R_LINX_HL_BSTART30_PCREL) {
                hwaddr section_start = load_base;
                if (target_sym->st_shndx < eh->e_shnum) {
                    section_start = sec_addr[target_sym->st_shndx];
                }
                target = linx_ensure_bstart(ram, ram_size, target, section_start);
                if (!linx_is_bstart(ram, ram_size, target)) {
                    error_setg(errp, "relocation target @ 0x%" HWADDR_PRIx
                               " does not point to BSTART instruction",
                               sym_addr[symidx]);
                    goto fail;
                }
                if (!linx_patch_hl_bstart_pcrel(ram, ram_size, patch_addr, target,
                                                (int64_t)rela[j].r_addend, errp)) {
                    goto fail;
                }
                continue;
            }
            
            /* Check what instruction is at the patch site */
            if (patch_addr + 4 > ram_size) {
                error_setg(errp, "relocation patch site out of bounds @ 0x%" HWADDR_PRIx,
                           patch_addr);
                goto fail;
            }
            patch_insn = ldl_le_p(ram + patch_addr);
            
            /* Determine if this is a code or data relocation based on instruction */
            target_is_code = ((patch_insn & 0x7fff) == 0x4001);  /* BSTART.CALL */
            
            if (target_is_code) {
                /* For code relocations, ensure target points to a BSTART instruction */
                hwaddr section_start = load_base;
                if (target_sym->st_shndx < eh->e_shnum) {
                    section_start = sec_addr[target_sym->st_shndx];
                }
                target = linx_ensure_bstart(ram, ram_size, target, section_start);
                if (!linx_is_bstart(ram, ram_size, target)) {
                    error_setg(errp, "relocation target @ 0x%" HWADDR_PRIx
                               " does not point to BSTART instruction",
                               sym_addr[symidx]);
                    goto fail;
                }
            }
            /* For data relocations (ADDTPC, LD.PCR, ST.PCR), target is used directly */

            if (!linx_patch_reloc(ram, ram_size, patch_addr, target,
                                  (int64_t)rela[j].r_addend, target_is_code, errp)) {
                goto fail;
            }
        }
    }

    *image_end = end;
    g_free(sym_addr);
    g_free(sec_addr);
    return true;

fail:
    g_free(sym_addr);
    g_free(sec_addr);
    return false;
}

static bool linx_load_elf32_exec(const uint8_t *buf, size_t len,
                                 uint8_t *ram, size_t ram_size,
                                 hwaddr load_base,
                                 hwaddr *entry, hwaddr *image_end,
                                 Error **errp)
{
    const Elf32_Ehdr *eh;
    const Elf32_Phdr *phdrs;
    hwaddr bias = 0;
    hwaddr min_load = (hwaddr)-1;
    hwaddr end = 0;
    hwaddr entry_addr;
    hwaddr entry_seg_start = load_base;

    size_t i;

    if (len < sizeof(*eh)) {
        error_setg(errp, "file too small for ELF header");
        return false;
    }
    eh = (const Elf32_Ehdr *)buf;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0) {
        error_setg(errp, "not an ELF file");
        return false;
    }
    if (eh->e_ident[EI_CLASS] != ELFCLASS32) {
        error_setg(errp, "expected ELF32 image");
        return false;
    }
    if (eh->e_ident[EI_DATA] != ELFDATA2LSB) {
        error_setg(errp, "unsupported ELF endianness");
        return false;
    }
    if (eh->e_type != ET_EXEC && eh->e_type != ET_DYN) {
        error_setg(errp, "expected ET_EXEC/ET_DYN image");
        return false;
    }

    if (eh->e_phoff == 0 || eh->e_phentsize != sizeof(Elf32_Phdr) ||
        eh->e_phnum == 0) {
        error_setg(errp, "invalid program header table");
        return false;
    }
    if ((size_t)eh->e_phoff + (size_t)eh->e_phentsize * eh->e_phnum > len) {
        error_setg(errp, "program header table out of bounds");
        return false;
    }
    phdrs = (const Elf32_Phdr *)(buf + eh->e_phoff);

    /* For ET_DYN, compute a load bias so the lowest segment starts at load_base. */
    for (i = 0; i < eh->e_phnum; i++) {
        const Elf32_Phdr *ph = &phdrs[i];
        hwaddr addr;

        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) {
            continue;
        }
        addr = ph->p_paddr ? (hwaddr)ph->p_paddr : (hwaddr)ph->p_vaddr;
        if (addr < min_load) {
            min_load = addr;
        }
    }
    if (eh->e_type == ET_DYN && min_load != (hwaddr)-1 && min_load < load_base) {
        bias = load_base - min_load;
    }

    for (i = 0; i < eh->e_phnum; i++) {
        const Elf32_Phdr *ph = &phdrs[i];
        hwaddr seg_addr;
        hwaddr seg_start;
        hwaddr seg_end;

        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) {
            continue;
        }

        if ((size_t)ph->p_offset + (size_t)ph->p_filesz > len) {
            error_setg(errp, "segment #%zu data out of bounds", i);
            return false;
        }
        if (ph->p_filesz > ph->p_memsz) {
            error_setg(errp, "segment #%zu filesz > memsz", i);
            return false;
        }

        seg_addr = ph->p_paddr ? (hwaddr)ph->p_paddr : (hwaddr)ph->p_vaddr;
        seg_start = seg_addr + bias;
        seg_end = seg_start + (hwaddr)ph->p_memsz;

        if ((uint64_t)seg_end > (uint64_t)ram_size) {
            error_setg(errp, "segment #%zu does not fit in RAM", i);
            return false;
        }

        if (ph->p_filesz) {
            memcpy(ram + seg_start, buf + ph->p_offset, ph->p_filesz);
        }
        if (ph->p_memsz > ph->p_filesz) {
            memset(ram + seg_start + ph->p_filesz, 0,
                   (size_t)(ph->p_memsz - ph->p_filesz));
        }

        if (seg_end > end) {
            end = seg_end;
        }
    }

    entry_addr = (hwaddr)eh->e_entry + bias;
    if (entry_addr == 0) {
        error_setg(errp, "invalid entry point (0x0)");
        return false;
    }
    if ((uint64_t)entry_addr + 2 > (uint64_t)ram_size) {
        error_setg(errp, "entry point out of RAM bounds @ 0x%" HWADDR_PRIx,
                   entry_addr);
        return false;
    }

    /* Find the segment containing the entry, to bound BSTART back-search. */
    for (i = 0; i < eh->e_phnum; i++) {
        const Elf32_Phdr *ph = &phdrs[i];
        hwaddr seg_addr;
        hwaddr seg_start;
        hwaddr seg_end;

        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) {
            continue;
        }
        seg_addr = ph->p_paddr ? (hwaddr)ph->p_paddr : (hwaddr)ph->p_vaddr;
        seg_start = seg_addr + bias;
        seg_end = seg_start + (hwaddr)ph->p_memsz;
        if (entry_addr >= seg_start && entry_addr < seg_end) {
            entry_seg_start = seg_start;
            break;
        }
    }

    *entry = linx_ensure_bstart(ram, ram_size, entry_addr, entry_seg_start);
    if (!linx_is_bstart(ram, ram_size, *entry)) {
        error_setg(errp, "entry does not point to BSTART instruction");
        return false;
    }

    *image_end = end;
    return true;
}

#endif

#if TARGET_LONG_BITS != 32
static bool linx_load_elf64_rel(const uint8_t *buf, size_t len,
                                uint8_t *ram, size_t ram_size,
                                hwaddr load_base,
                                hwaddr *entry, hwaddr *image_end,
                                Error **errp)
{
    const Elf64_Ehdr *eh;
    const Elf64_Shdr *shdrs;
    const Elf64_Shdr *shstr_sh;
    hwaddr *sec_addr;
    hwaddr cur;
    hwaddr end = load_base;

    const Elf64_Shdr *symtab_sh = NULL;
    const Elf64_Shdr *strtab_sh = NULL;
    const Elf64_Sym *syms = NULL;
    size_t nsyms = 0;
    hwaddr *sym_addr = NULL;
    bool found_start = false;

    size_t i;

    if (len < sizeof(*eh)) {
        error_setg(errp, "file too small for ELF header");
        return false;
    }
    eh = (const Elf64_Ehdr *)buf;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0) {
        error_setg(errp, "not an ELF file");
        return false;
    }
    if (eh->e_ident[EI_CLASS] != ELFCLASS64) {
        error_setg(errp, "expected ELF64 object");
        return false;
    }
    if (eh->e_ident[EI_DATA] != ELFDATA2LSB) {
        error_setg(errp, "unsupported ELF endianness");
        return false;
    }
    if (eh->e_type != ET_REL) {
        error_setg(errp, "expected ET_REL object");
        return false;
    }

    if (eh->e_shoff == 0 || eh->e_shentsize != sizeof(Elf64_Shdr) ||
        eh->e_shnum == 0) {
        error_setg(errp, "invalid section header table");
        return false;
    }
    if ((size_t)eh->e_shoff + (size_t)eh->e_shentsize * eh->e_shnum > len) {
        error_setg(errp, "section header table out of bounds");
        return false;
    }

    shdrs = (const Elf64_Shdr *)(buf + eh->e_shoff);
    if (eh->e_shstrndx == SHN_UNDEF || eh->e_shstrndx >= eh->e_shnum) {
        error_setg(errp, "invalid shstrndx");
        return false;
    }
    shstr_sh = &shdrs[eh->e_shstrndx];
    if ((size_t)shstr_sh->sh_offset + shstr_sh->sh_size > len) {
        error_setg(errp, "shstrtab out of bounds");
        return false;
    }

    sec_addr = g_new0(hwaddr, eh->e_shnum);
    cur = load_base;
    hwaddr bss_cur = LINX_BSS_BASE;
    hwaddr bss_end = bss_cur;

    for (i = 0; i < eh->e_shnum; i++) {
        const Elf64_Shdr *sh = &shdrs[i];
        hwaddr align;
        hwaddr *cursor = &cur;

        if (!(sh->sh_flags & SHF_ALLOC) || sh->sh_size == 0) {
            continue;
        }
        align = sh->sh_addralign ? sh->sh_addralign : 1;
        if ((align & (align - 1)) != 0) {
            error_setg(errp, "invalid section alignment for #%zu", i);
            goto fail;
        }

        if (sh->sh_type == SHT_NOBITS) {
            bss_cur = linx_align_up(bss_cur, align);
            if (bss_cur + sh->sh_size <= load_base) {
                cursor = &bss_cur;
            }
        }

        *cursor = linx_align_up(*cursor, align);
        if ((size_t)*cursor + sh->sh_size > ram_size) {
            error_setg(errp, "section #%zu does not fit in RAM", i);
            goto fail;
        }

        sec_addr[i] = *cursor;
        if (sh->sh_type == SHT_NOBITS) {
            memset(ram + *cursor, 0, sh->sh_size);
        } else {
            if ((size_t)sh->sh_offset + sh->sh_size > len) {
                error_setg(errp, "section #%zu data out of bounds", i);
                goto fail;
            }
            memcpy(ram + *cursor, buf + sh->sh_offset, sh->sh_size);
        }

        *cursor += sh->sh_size;
        if (cursor == &bss_cur) {
            if (bss_cur > bss_end) {
                bss_end = bss_cur;
            }
        } else {
            if (cur > end) {
                end = cur;
            }
        }
    }
    if (bss_end > end) {
        end = bss_end;
    }

    for (i = 0; i < eh->e_shnum; i++) {
        const Elf64_Shdr *sh = &shdrs[i];
        if (sh->sh_type == SHT_SYMTAB) {
            symtab_sh = sh;
            if (sh->sh_link < eh->e_shnum) {
                strtab_sh = &shdrs[sh->sh_link];
            }
            break;
        }

    }
    if (!symtab_sh || !strtab_sh || strtab_sh->sh_type != SHT_STRTAB) {
        error_setg(errp, "missing .symtab/.strtab");
        goto fail;
    }
    if ((size_t)symtab_sh->sh_offset + symtab_sh->sh_size > len ||
        (size_t)strtab_sh->sh_offset + strtab_sh->sh_size > len) {
        error_setg(errp, "symtab/strtab out of bounds");
        goto fail;
    }

    syms = (const Elf64_Sym *)(buf + symtab_sh->sh_offset);
    nsyms = symtab_sh->sh_size / sizeof(Elf64_Sym);
    sym_addr = g_new0(hwaddr, nsyms);

    /* Allocate SHN_COMMON symbols in the BSS window when possible. */
    for (i = 0; i < nsyms; i++) {
        const Elf64_Sym *sym = &syms[i];
        hwaddr align;

        if (sym->st_shndx != SHN_COMMON || sym->st_size == 0) {
            continue;
        }

        align = sym->st_value ? sym->st_value : 1;
        if ((align & (align - 1)) != 0) {
            error_setg(errp, "invalid common symbol alignment for %s",
                       linx_elf64_sym_name(buf, len, strtab_sh, sym));
            goto fail;
        }

        bss_end = linx_align_up(bss_end, align);
        if (bss_end + sym->st_size > load_base) {
            end = linx_align_up(end, align);
            bss_end = end;
        }

        if ((size_t)bss_end + sym->st_size > ram_size) {
            error_setg(errp, "common symbol %s does not fit in RAM",
                       linx_elf64_sym_name(buf, len, strtab_sh, sym));
            goto fail;
        }

        sym_addr[i] = bss_end;
        memset(ram + bss_end, 0, sym->st_size);
        bss_end += sym->st_size;
        if (bss_end > end) {
            end = bss_end;
        }
    }

    for (i = 0; i < nsyms; i++) {
        const Elf64_Sym *sym = &syms[i];
        if (sym->st_shndx == SHN_UNDEF) {
            continue;
        }
        if (sym->st_shndx == SHN_ABS) {
            sym_addr[i] = sym->st_value;
            continue;
        }
        if (sym->st_shndx < eh->e_shnum) {
            sym_addr[i] = sec_addr[sym->st_shndx] + sym->st_value;
        }
    }

    *entry = 0;
    for (i = 0; i < nsyms; i++) {
        const Elf64_Sym *sym = &syms[i];
        const char *name;

        if (sym->st_name >= strtab_sh->sh_size) {
            continue;
        }
        name = (const char *)(buf + strtab_sh->sh_offset + sym->st_name);
        if (!strcmp(name, "_start")) {
            hwaddr entry_addr = sym_addr[i];
            /* Find the section containing this symbol */
            hwaddr section_start = load_base;
            if (sym->st_shndx < eh->e_shnum) {
                section_start = sec_addr[sym->st_shndx];
            }
            /* Ensure entry point points to a BSTART instruction */
            *entry = linx_ensure_bstart(ram, ram_size, entry_addr, section_start);
            if (*entry != entry_addr && !linx_is_bstart(ram, ram_size, *entry)) {
                error_setg(errp, "entry symbol _start does not point to BSTART instruction");
                goto fail;
            }
            found_start = true;
            break;
        }
    }
    if (!found_start) {
        error_setg(errp, "entry symbol _start not found");
        goto fail;
    }

    for (i = 0; i < eh->e_shnum; i++) {
        const Elf64_Shdr *relsh = &shdrs[i];
        const Elf64_Rela *rela;
        size_t nrela;
        size_t j;
        hwaddr base;

        if (relsh->sh_type != SHT_RELA || relsh->sh_info >= eh->e_shnum) {
            continue;
        }
        if ((size_t)relsh->sh_offset + relsh->sh_size > len) {
            error_setg(errp, "relocation section out of bounds");
            goto fail;
        }

        base = sec_addr[relsh->sh_info];
        rela = (const Elf64_Rela *)(buf + relsh->sh_offset);
        nrela = relsh->sh_size / sizeof(Elf64_Rela);

        for (j = 0; j < nrela; j++) {
            unsigned symidx = ELF64_R_SYM(rela[j].r_info);
            unsigned rtype = ELF64_R_TYPE(rela[j].r_info);
            hwaddr target;
            hwaddr patch_addr;
            uint32_t patch_insn;
            const Elf64_Sym *target_sym = NULL;
            bool target_is_code;

            if (symidx >= nsyms) {
                error_setg(errp, "invalid relocation symbol index %u (nsyms=%zu)",
                           symidx, nsyms);
                goto fail;
            }
            target_sym = &syms[symidx];
            if (target_sym->st_shndx == SHN_UNDEF) {
                error_setg(errp, "undefined symbol %s (index %u) in relocation",
                           linx_elf64_sym_name(buf, len, strtab_sh, target_sym), symidx);
                goto fail;
            }
            if (target_sym->st_shndx < eh->e_shnum && sec_addr[target_sym->st_shndx] == 0) {
                error_setg(errp, "symbol %s refers to non-alloc section #%u",
                           linx_elf64_sym_name(buf, len, strtab_sh, target_sym),
                           target_sym->st_shndx);
                goto fail;
            }

            target = sym_addr[symidx];
            patch_addr = base + rela[j].r_offset;

            if (rtype == R_LINX_64) {
                if (patch_addr + 8 > ram_size) {
                    error_setg(errp, "relocation patch out of RAM bounds @ 0x%" HWADDR_PRIx,
                               patch_addr);
                    goto fail;
                }
                stq_le_p(ram + patch_addr, (uint64_t)(target + rela[j].r_addend));
                continue;
            } else if (rtype == R_LINX_32) {
                if (patch_addr + 4 > ram_size) {
                    error_setg(errp, "relocation patch out of RAM bounds @ 0x%" HWADDR_PRIx,
                               patch_addr);
                    goto fail;
                }
                stl_le_p(ram + patch_addr, (uint32_t)(target + rela[j].r_addend));
                continue;
            }

            if (rtype == R_LINX_HL_PCR29_LOAD) {
                if (!linx_patch_hl_ld_pcr(ram, ram_size, patch_addr, target,
                                          rela[j].r_addend, errp)) {
                    goto fail;
                }
                continue;
            } else if (rtype == R_LINX_HL_PCR29_STORE) {
                if (!linx_patch_hl_st_pcr(ram, ram_size, patch_addr, target,
                                          rela[j].r_addend, errp)) {
                    goto fail;
                }
                continue;
            } else if (rtype == R_LINX_HL_BSTART30_PCREL) {
                hwaddr section_start = load_base;
                if (target_sym->st_shndx < eh->e_shnum) {
                    section_start = sec_addr[target_sym->st_shndx];
                }
                target = linx_ensure_bstart(ram, ram_size, target, section_start);
                if (!linx_is_bstart(ram, ram_size, target)) {
                    error_setg(errp, "relocation target @ 0x%" HWADDR_PRIx
                               " does not point to BSTART instruction",
                               sym_addr[symidx]);
                    goto fail;
                }
                if (!linx_patch_hl_bstart_pcrel(ram, ram_size, patch_addr, target,
                                                rela[j].r_addend, errp)) {
                    goto fail;
                }
                continue;
            }
            
            /* Check what instruction is at the patch site */
            if (patch_addr + 4 > ram_size) {
                error_setg(errp, "relocation patch site out of bounds @ 0x%" HWADDR_PRIx,
                           patch_addr);
                goto fail;
            }
            patch_insn = ldl_le_p(ram + patch_addr);
            
            /* Determine if this is a code or data relocation based on instruction */
            target_is_code = ((patch_insn & 0x7fff) == 0x4001);  /* BSTART.CALL */
            
            if (target_is_code) {
                /* For code relocations, ensure target points to a BSTART instruction */
                hwaddr section_start = load_base;
                if (target_sym->st_shndx < eh->e_shnum) {
                    section_start = sec_addr[target_sym->st_shndx];
                }
                target = linx_ensure_bstart(ram, ram_size, target, section_start);
                if (!linx_is_bstart(ram, ram_size, target)) {
                    error_setg(errp, "relocation target @ 0x%" HWADDR_PRIx
                               " does not point to BSTART instruction",
                               sym_addr[symidx]);
                    goto fail;
                }
            }
            /* For data relocations (ADDTPC, LD.PCR, ST.PCR), target is used directly */

            if (!linx_patch_reloc(ram, ram_size, patch_addr, target,
                                  rela[j].r_addend, target_is_code, errp)) {
                goto fail;
            }
        }
    }

    *image_end = end;
    g_free(sym_addr);
    g_free(sec_addr);
    return true;

fail:
    g_free(sym_addr);
    g_free(sec_addr);
    return false;
}

static bool linx_load_elf64_exec(const uint8_t *buf, size_t len,
                                 uint8_t *ram, size_t ram_size,
                                 hwaddr load_base,
                                 hwaddr *entry, hwaddr *image_end,
                                 Error **errp)
{
    const Elf64_Ehdr *eh;
    const Elf64_Phdr *phdrs;
    hwaddr bias = 0;
    hwaddr min_load = (hwaddr)-1;
    hwaddr end = 0;
    hwaddr entry_addr;
    hwaddr entry_seg_start = load_base;

    size_t i;

    if (len < sizeof(*eh)) {
        error_setg(errp, "file too small for ELF header");
        return false;
    }
    eh = (const Elf64_Ehdr *)buf;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0) {
        error_setg(errp, "not an ELF file");
        return false;
    }
    if (eh->e_ident[EI_CLASS] != ELFCLASS64) {
        error_setg(errp, "expected ELF64 image");
        return false;
    }
    if (eh->e_ident[EI_DATA] != ELFDATA2LSB) {
        error_setg(errp, "unsupported ELF endianness");
        return false;
    }
    if (eh->e_type != ET_EXEC && eh->e_type != ET_DYN) {
        error_setg(errp, "expected ET_EXEC/ET_DYN image");
        return false;
    }

    if (eh->e_phoff == 0 || eh->e_phentsize != sizeof(Elf64_Phdr) ||
        eh->e_phnum == 0) {
        error_setg(errp, "invalid program header table");
        return false;
    }
    if ((size_t)eh->e_phoff + (size_t)eh->e_phentsize * eh->e_phnum > len) {
        error_setg(errp, "program header table out of bounds");
        return false;
    }
    phdrs = (const Elf64_Phdr *)(buf + eh->e_phoff);

    /* For ET_DYN, compute a load bias so the lowest segment starts at load_base. */
    for (i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *ph = &phdrs[i];
        hwaddr addr;

        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) {
            continue;
        }
        addr = ph->p_paddr ? (hwaddr)ph->p_paddr : (hwaddr)ph->p_vaddr;
        if (addr < min_load) {
            min_load = addr;
        }
    }
    if (eh->e_type == ET_DYN && min_load != (hwaddr)-1 && min_load < load_base) {
        bias = load_base - min_load;
    }

    for (i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *ph = &phdrs[i];
        hwaddr seg_addr;
        hwaddr seg_start;
        hwaddr seg_end;

        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) {
            continue;
        }

        if ((size_t)ph->p_offset + (size_t)ph->p_filesz > len) {
            error_setg(errp, "segment #%zu data out of bounds", i);
            return false;
        }
        if (ph->p_filesz > ph->p_memsz) {
            error_setg(errp, "segment #%zu filesz > memsz", i);
            return false;
        }

        seg_addr = ph->p_paddr ? (hwaddr)ph->p_paddr : (hwaddr)ph->p_vaddr;
        seg_start = seg_addr + bias;
        seg_end = seg_start + (hwaddr)ph->p_memsz;

        if ((uint64_t)seg_end > (uint64_t)ram_size) {
            error_setg(errp, "segment #%zu does not fit in RAM", i);
            return false;
        }

        if (ph->p_filesz) {
            memcpy(ram + seg_start, buf + ph->p_offset, ph->p_filesz);
        }
        if (ph->p_memsz > ph->p_filesz) {
            memset(ram + seg_start + ph->p_filesz, 0,
                   (size_t)(ph->p_memsz - ph->p_filesz));
        }

        if (seg_end > end) {
            end = seg_end;
        }
    }

    entry_addr = (hwaddr)eh->e_entry + bias;
    if (entry_addr == 0) {
        error_setg(errp, "invalid entry point (0x0)");
        return false;
    }
    if ((uint64_t)entry_addr + 2 > (uint64_t)ram_size) {
        error_setg(errp, "entry point out of RAM bounds @ 0x%" HWADDR_PRIx,
                   entry_addr);
        return false;
    }

    /* Find the segment containing the entry, to bound BSTART back-search. */
    for (i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *ph = &phdrs[i];
        hwaddr seg_addr;
        hwaddr seg_start;
        hwaddr seg_end;

        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) {
            continue;
        }
        seg_addr = ph->p_paddr ? (hwaddr)ph->p_paddr : (hwaddr)ph->p_vaddr;
        seg_start = seg_addr + bias;
        seg_end = seg_start + (hwaddr)ph->p_memsz;
        if (entry_addr >= seg_start && entry_addr < seg_end) {
            entry_seg_start = seg_start;
            break;
        }
    }

    *entry = linx_ensure_bstart(ram, ram_size, entry_addr, entry_seg_start);
    if (!linx_is_bstart(ram, ram_size, *entry)) {
        error_setg(errp, "entry does not point to BSTART instruction");
        return false;
    }

    *image_end = end;
    return true;
}

#endif

static bool linx_load_elf(const char *filename,
                          uint8_t *ram, size_t ram_size,
                          hwaddr load_base,
                          hwaddr *entry, hwaddr *image_end,
                          Error **errp)
{
    GError *gerr = NULL;
    uint8_t *buf = NULL;
    gsize len = 0;
    bool ok = false;

    if (!g_file_get_contents(filename, (gchar **)&buf, &len, &gerr)) {
        error_setg(errp, "unable to read %s: %s", filename, gerr->message);
        g_clear_error(&gerr);
        return false;
    }

#if TARGET_LONG_BITS == 32
    if (len >= sizeof(Elf32_Ehdr) && ((const Elf32_Ehdr *)buf)->e_type == ET_REL) {
        ok = linx_load_elf32_rel(buf, len, ram, ram_size, load_base,
                                 entry, image_end, errp);
    } else {
        ok = linx_load_elf32_exec(buf, len, ram, ram_size, load_base,
                                  entry, image_end, errp);
    }
#else
    if (len >= sizeof(Elf64_Ehdr) && ((const Elf64_Ehdr *)buf)->e_type == ET_REL) {
        ok = linx_load_elf64_rel(buf, len, ram, ram_size, load_base,
                                 entry, image_end, errp);
    } else {
        ok = linx_load_elf64_exec(buf, len, ram, ram_size, load_base,
                                  entry, image_end, errp);
    }
#endif

    g_free(buf);
    return ok;
}

static void linx_virt_reset(void *opaque)
{
    LinxVirtMachineState *s = opaque;
    CPULinxState *env = &s->cpu->env;
    CPUState *cs = CPU(s->cpu);

    cpu_reset(cs);

    if (s->entry == 0) {
        error_report("linx virt: invalid entry point (0x0)");
        exit(1);
    }

    if (linx_virt_debug_enabled()) {
        fprintf(stderr, "linx virt: reset entry=0x%" HWADDR_PRIx " sp=0x%" HWADDR_PRIx
                        " tramp=0x%" HWADDR_PRIx "\n",
                s->entry, s->initial_sp, s->exit_trampoline);
        fflush(stderr);
    }

    // Quiet boot - don't print entry address to avoid mixing with UART output
    // qemu_log_mask(LOG_TRACE, "linx virt: entry=0x%" HWADDR_PRIx " sp=0x%" HWADDR_PRIx "\n",
    //               s->entry, s->initial_sp);

    env->pc = s->entry;
    env->gpr[LINX_REG_SP] = s->initial_sp;
    env->gpr[LINX_REG_RA] = s->exit_trampoline;
    env->gpr[LINX_REG_ZERO] = 0;
    env->gpr[LINX_REG_A0] = 0;  /* hart id */
    env->gpr[LINX_REG_A1] = s->fdt_addr; /* DTB/FDT physical address */
    env->gpr[LINX_REG_A2] = 0;
}

static void linx_virt_init(MachineState *machine)
{
    LinxVirtMachineState *s = LINX_VIRT_MACHINE(machine);
    uint8_t *ram;
    hwaddr entry = 0;
    hwaddr image_end = 0;
    hwaddr cur;
    hwaddr initrd_base = 0;
    hwaddr initrd_size = 0;
    hwaddr fdt_addr = 0;
    int fdt_alloc_size = 0;
    int fdt_size;
    void *fdt = NULL;
    int ret;

    hwaddr load_base = 0x10000;
    hwaddr tramp;
    hwaddr sp;

    if (!machine->kernel_filename) {
        error_report("linx virt: missing -kernel <linxisa kernel image>");
        exit(1);
    }

    if (linx_virt_debug_enabled()) {
        fprintf(stderr, "linx virt: loading kernel %s\n", machine->kernel_filename);
        fflush(stderr);
    }

    if (!machine->ram) {
        error_report("linx virt: machine RAM not initialized");
        exit(1);
    }

    memory_region_add_subregion(get_system_memory(), 0, machine->ram);

    s->cpu = LINX_CPU(cpu_create(machine->cpu_type));

    /* Initialize UART */
    linx_uart_init(&s->uart, s->cpu);

    ram = memory_region_get_ram_ptr(machine->ram);
    if (!linx_load_elf(machine->kernel_filename, ram, machine->ram_size,
                       load_base, &entry, &image_end, &error_fatal)) {
        exit(1);
    }

    if (linx_virt_debug_enabled()) {
        fprintf(stderr, "linx virt: loaded entry=0x%" HWADDR_PRIx " image_end=0x%" HWADDR_PRIx "\n",
                entry, image_end);
        fflush(stderr);
    }

    cur = linx_align_up(image_end, 0x1000);

    if (machine->initrd_filename) {
        GError *gerr = NULL;
        uint8_t *buf = NULL;
        gsize len = 0;

        if (!g_file_get_contents(machine->initrd_filename, (gchar **)&buf, &len,
                                 &gerr)) {
            error_report("linx virt: unable to read initrd %s: %s",
                         machine->initrd_filename, gerr->message);
            g_clear_error(&gerr);
            exit(1);
        }

        initrd_base = cur;
        initrd_size = len;
        if ((size_t)initrd_base + initrd_size > machine->ram_size) {
            error_report("linx virt: initrd does not fit in RAM");
            exit(1);
        }
        memcpy(ram + initrd_base, buf, initrd_size);
        g_free(buf);

        cur = linx_align_up(initrd_base + initrd_size, 0x1000);

        if (linx_virt_debug_enabled()) {
            fprintf(stderr, "linx virt: loaded initrd=%s @ 0x%" HWADDR_PRIx " size=0x%" HWADDR_PRIx "\n",
                    machine->initrd_filename, initrd_base, initrd_size);
            fflush(stderr);
        }
    }

    fdt = linx_virt_get_fdt(machine, machine->ram_size,
                            initrd_base, initrd_size, &fdt_alloc_size);
    fdt_size = fdt_totalsize(fdt);
    if (fdt_size <= 0) {
        error_report("linx virt: invalid device tree");
        exit(1);
    }
    ret = fdt_pack(fdt);
    g_assert(ret == 0);

    fdt_addr = linx_align_up(cur, 0x10);
    if ((size_t)fdt_addr + (size_t)fdt_size > machine->ram_size) {
        error_report("linx virt: FDT does not fit in RAM");
        exit(1);
    }
    memcpy(ram + fdt_addr, fdt, fdt_size);
    machine->fdt = fdt;
    qemu_register_reset_nosnapshotload(qemu_fdt_randomize_seeds,
                                      ram + fdt_addr);

    cur = linx_align_up(fdt_addr + (hwaddr)fdt_size, 0x10);
    if (cur > image_end) {
        image_end = cur;
    }

    if (linx_virt_debug_enabled()) {
        fprintf(stderr, "linx virt: fdt @ 0x%" HWADDR_PRIx " size=0x%x\n",
                fdt_addr, fdt_size);
        fflush(stderr);
    }

    tramp = (machine->ram_size - 8) & ~0xfULL;
    sp = (tramp - 0x10000) & ~0xfULL;

    if (image_end > sp) {
        error_report("linx virt: RAM too small (image_end=0x%" HWADDR_PRIx
                     " sp=0x%" HWADDR_PRIx ")", image_end, sp);
        exit(1);
    }

    /* Simple exit trampoline: C.BSTART (FALL), EBREAK, C.BSTOP. */
    {
        static const uint8_t tramp_code[8] = {
            0x00, 0x08,             /* C.BSTART (FALL) */
            0x2b, 0x10, 0x10, 0x00, /* EBREAK imm=0 */
            0x00, 0x00,             /* C.BSTOP */
        };
        memcpy(ram + tramp, tramp_code, sizeof(tramp_code));
    }

    s->entry = entry;
    s->initial_sp = sp;
    s->exit_trampoline = tramp;
    s->fdt_addr = fdt_addr;

    qemu_register_reset(linx_virt_reset, s);
}

static void linx_virt_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "QEMU LinxISA Virtual Machine";
    mc->init = linx_virt_init;
    mc->default_cpu_type = TYPE_LINX_CPU_LINX;
    mc->default_cpus = 1;
    mc->max_cpus = 1;
    mc->default_ram_id = "linx.virt.ram";
    mc->default_ram_size = 128 * MiB;
}

static const TypeInfo linx_virt_machine_info = {
    .name = TYPE_LINX_VIRT_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(LinxVirtMachineState),
    .class_init = linx_virt_machine_class_init,
};

static void linx_virt_machine_register_types(void)
{
    type_register_static(&linx_virt_machine_info);
}

type_init(linx_virt_machine_register_types)
