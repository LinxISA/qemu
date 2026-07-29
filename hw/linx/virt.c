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
#include "hw/core/cpu.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/virtio/virtio-mmio.h"
#include "system/address-spaces.h"
#include "system/device_tree.h"
#include "system/memory.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "elf.h"
#include "chardev/char.h"
#include "chardev/char-fe.h"
#include "qemu/qemu-print.h"
#include "system/system.h"

#include "cpu.h"
#include "trace.h"

#include <libfdt.h>

static bool linx_virt_print_insn_count_enabled(void)
{
    const char *v = getenv("LINX_PRINT_INSN_COUNT");
    return v && v[0] && strcmp(v, "0") != 0;
}

static bool linx_test_finisher_enabled(void)
{
    const char *v = getenv("LINX_VIRT_TEST_FINISHER");

    return v && v[0] && strcmp(v, "0") != 0;
}

static bool linx_virtio_mmio_debug_enabled(void)
{
    const char *v = getenv("LINX_VIRTIO_MMIO_DEBUG");

    if (!v) {
        v = getenv("LINX_QEMU_VIRTIO_MMIO_DEBUG");
    }
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

/* Canonical AVS/system test finisher MMIO */
#define LINX_TEST_FINISHER_SIZE 0x4
#define LINX_CROSS_MODEL_DUMP_MAX (16 * MiB)

/* Linx virt-uart MMIO register layout (offsets from LINX_UART_BASE). */
#define LINX_UART_DATA_REG 0x0
#define LINX_UART_STATUS_REG 0x4
#define LINX_UART_STATUS_TX_READY 0x1
#define LINX_UART_STATUS_RX_READY 0x2

#define LINX_UART_RX_BUFSZ 256

/* Virtio-mmio transport (single slot for bring-up disk boot). */
#define LINX_VIRTIO_MMIO_BASE 0x30001000
#define LINX_VIRTIO_MMIO_STRIDE 0x200
#define LINX_VIRTIO_MMIO_SIZE 0x200
#define LINX_VIRTIO_MMIO_IRQ_BASE 8
#define LINX_VIRTIO_MMIO_COUNT 4
#define LINX_VIRTIO_MMIO_TOTAL_SIZE \
    (LINX_VIRTIO_MMIO_STRIDE * (LINX_VIRTIO_MMIO_COUNT - 1) + \
     LINX_VIRTIO_MMIO_SIZE)

/* Linux allocates normal pages at 4 KiB granularity during bring-up. */
#define LINX_MMIO_RESERVED_ALIGN 0x1000

/* The Linx Linux timer driver reads SSR_TIMER_TIME as a free-running cycle
 * source. QEMU currently models that register with QEMU_CLOCK_VIRTUAL in ns,
 * so advertise a 1 GHz timebase in the generated DT.
 */
#define LINX_TIMEBASE_FREQUENCY 1000000000U

/* ACR-scoped SSR low-12 indices used for external IRQ injection. */
#define LINX_SSR_IPENDING 0xF08

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

/* Current ELF relocation ABI from LLVM's ELFRelocs/LinxISA.def. */
#define R_LINX_NONE 0
#define R_LINX_B12_PCREL 1
#define R_LINX_J22_PCREL 2
#define R_LINX_CBSTART12_PCREL 3
#define R_LINX_B17_PCREL 4
#define R_LINX_HL_BSTART30_PCREL 5
#define R_LINX_CSETRET5_PCREL 6
#define R_LINX_SETRET20_PCREL 7
#define R_LINX_HL_SETRET32_PCREL 8
#define R_LINX_RELATIVE 9
#define R_LINX_64 10
#define R_LINX_32 11
#define R_LINX_JUMP_SLOT 12
#define R_LINX_GLOB_DAT 13
#define R_LINX_COPY 14
#define R_LINX_PCREL_HI20 15
#define R_LINX_B17_PLT 16
#define R_LINX_LO12 17
#define R_LINX_PCR17_LOAD 18
#define R_LINX_PCR17_STORE 19
#define R_LINX_HL_PCR29_LOAD 20
#define R_LINX_HL_PCR29_STORE 21
#define R_LINX_B25_PCREL 22
#define R_LINX_GOT_HI20 23
#define R_LINX_GOT_LO12 24
#define R_LINX_32_PCREL 25
#define R_LINX_TLS_DTPMOD64 28
#define R_LINX_TLS_DTPREL64 29
#define R_LINX_TLS_TPREL64 30
#define R_LINX_TLSDESC 31
#define R_LINX_IRELATIVE 32

static inline uint32_t linx_set_lo12_i(uint32_t insn, uint32_t imm)
{
    return (insn & 0x000fffffU) | (imm << 20);
}

/* Simple UART state */
typedef struct LinxUARTState {
    MemoryRegion mmio;
    MemoryRegion finisher_mmio;
    QemuMutex lock;
    LinxCPU *cpu;
    bool *cross_model_dump_pending;

    CharFrontend chr;

    uint8_t rx_buf[LINX_UART_RX_BUFSZ];
    uint32_t rx_head;
    uint32_t rx_tail;
} LinxUARTState;

static void linx_finish_guest(LinxUARTState *s, uint64_t value)
{
    uint64_t status = value & UINT64_C(0xffff);
    int exit_code;

    trace_linx_virt_exit_write(value);
    if (status == LINX_VIRT_FINISHER_RESET) {
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
        if (s->cpu) {
            cpu_exit(CPU(s->cpu));
        }
        return;
    }
    if (status == LINX_VIRT_FINISHER_PASS) {
        exit_code = 0;
    } else if (status == LINX_VIRT_FINISHER_FAIL) {
        exit_code = 1;
    } else {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "Linx: ignored invalid finisher value 0x%" PRIx64 "\n",
                      value);
        return;
    }

    if (s->cross_model_dump_pending) {
        *s->cross_model_dump_pending = true;
    }

    if (s->cpu) {
        CPULinxState *env = &s->cpu->env;
        if (linx_virt_print_insn_count_enabled()) {
            fprintf(stderr, "LINX_INSN_COUNT=%" PRIu64 "\n", env->insn_count);
            fflush(stderr);
        }
        /* Stop commit tracing after the exit store commits (difftest). */
        env->commit_trace.stop_after_commit = 1;
        env->minst_trace.stop_after_commit = 1;
    }
    qemu_system_shutdown_request_with_code(SHUTDOWN_CAUSE_GUEST_SHUTDOWN,
                                           exit_code);
    if (s->cpu) {
        cpu_exit(CPU(s->cpu));
    }
}

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

    trace_linx_virt_uart_can_receive(space);
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
        trace_linx_virt_uart_receive((uint32_t)buf[i], s->rx_head, s->rx_tail);
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
        trace_linx_virt_uart_read_data((uint32_t)c, s->rx_head, s->rx_tail);
        qemu_mutex_unlock(&s->lock);

        qemu_chr_fe_accept_input(&s->chr);
        return c;
    } else if (addr >= LINX_UART_STATUS_REG && addr < LINX_UART_STATUS_REG + 4) {
        uint64_t st = LINX_UART_STATUS_TX_READY;

        qemu_chr_fe_accept_input(&s->chr);
        qemu_mutex_lock(&s->lock);
        if (s->rx_head != s->rx_tail) {
            st |= LINX_UART_STATUS_RX_READY;
        }
        qemu_mutex_unlock(&s->lock);

        trace_linx_virt_uart_read_status(st);
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

    if (addr != 0) {
        return;  /* Ignore non-data writes */
    }

    qemu_mutex_lock(&s->lock);
    c = (unsigned char)(value & 0xFF);
    trace_linx_virt_uart_write(s->cpu ? s->cpu->env.pc : 0, (uint32_t)c, (uint32_t)c);
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

static uint64_t linx_finisher_read(void *opaque, hwaddr addr, unsigned size)
{
    (void)opaque;
    (void)addr;
    (void)size;
    return 0;
}

static void linx_finisher_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned size)
{
    LinxUARTState *s = opaque;
    (void)size;
    if (addr == 0) {
        linx_finish_guest(s, value);
    }
}

static const MemoryRegionOps linx_finisher_ops = {
    .read = linx_finisher_read,
    .write = linx_finisher_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
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
    if (linx_test_finisher_enabled()) {
        memory_region_init_io(&s->finisher_mmio, NULL, &linx_finisher_ops, s,
                              "linx-test-finisher", LINX_TEST_FINISHER_SIZE);
        memory_region_add_subregion_overlap(get_system_memory(),
                                            LINX_VIRT_FINISHER_ADDR,
                                            &s->finisher_mmio, 1);
    }
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
    qemu_chr_fe_accept_input(&s->chr);
}

static void linx_virt_set_irq(void *opaque, int irq, int level)
{
    LinxCPU *cpu = opaque;
    CPUState *cs = CPU(cpu);
    CPULinxState *env = &cpu->env;
    const uint32_t irq_id = (uint32_t)irq & 63u;
    const uint64_t bit = (1ull << irq_id);

    if (level) {
        env->irq_level_acr[1] |= bit;
        env->ssr_acr[1][LINX_SSR_IPENDING] |= bit;
        cpu_interrupt(cs, CPU_INTERRUPT_HARD);
    } else {
        /*
         * Keep IPENDING latched until the guest EOIs the IRQ via EOIEI.
         * This avoids dropping edge-like pulses before trap delivery.
         */
        env->irq_level_acr[1] &= ~bit;
        if (env->ssr_acr[1][LINX_SSR_IPENDING] == 0) {
            cpu_reset_interrupt(cs, CPU_INTERRUPT_HARD);
        }
    }
}

#define TYPE_LINX_VIRT_MACHINE MACHINE_TYPE_NAME("virt")
OBJECT_DECLARE_SIMPLE_TYPE(LinxVirtMachineState, LINX_VIRT_MACHINE)

typedef struct LinxVirtMachineState {
    MachineState parent_obj;

    LinxCPU *cpu[4];
    unsigned pe_count;

    hwaddr entry;
    bool entry_valid;
    hwaddr initial_sp;
    hwaddr exit_trampoline;
    hwaddr fdt_addr;

    qemu_irq virtio_irq[LINX_VIRTIO_MMIO_COUNT];
    LinxUARTState uart;

    /* DFX: RAM memwatch overlay (catches CPU + DMA writes). */
    uint64_t dfx_memwatch_addr;
    uint32_t dfx_memwatch_len;
    bool dfx_memwatch_stop;
    MemoryRegion dfx_memwatch;
    uint8_t *dfx_ram_ptr;

    char *cross_model_dump;
    uint64_t cross_model_address;
    uint32_t cross_model_size;
    bool cross_model_dump_pending;
    Notifier cross_model_shutdown_notifier;
} LinxVirtMachineState;

static hwaddr linx_align_up(hwaddr v, hwaddr align)
{
    if (align <= 1) {
        return v;
    }
    return (v + align - 1) & ~(align - 1);
}

#define LINX_LINUX_PMD_ALIGN 0x200000ULL

static void linx_set_body_ranges(CPULinxState *env,
                                 LinxBodyRange *ranges,
                                 uint32_t count)
{
    if (!env) {
        g_free(ranges);
        return;
    }
    g_free(env->body_ranges);
    env->body_ranges = ranges;
    env->body_range_count = count;
}

static void linx_set_call_continuations(CPULinxState *env,
                                        uint64_t *targets,
                                        uint32_t count)
{
    if (!env) {
        g_free(targets);
        return;
    }
    g_free(env->call_continuations);
    env->call_continuations = targets;
    env->call_continuation_count = count;
}

static bool linx_record_call_continuation(uint64_t **targets,
                                          uint32_t *count,
                                          uint64_t target,
                                          Error **errp)
{
    uint32_t i;
    uint64_t *grown;

    for (i = 0; i < *count; i++) {
        if ((*targets)[i] == target) {
            return true;
        }
    }

    grown = g_renew(uint64_t, *targets, *count + 1);
    if (!grown) {
        error_setg(errp, "failed to record call continuation target");
        return false;
    }
    grown[*count] = target;
    *targets = grown;
    *count += 1;
    return true;
}

static bool linx_body_end_name_to_start_name(const char *name,
                                             char *out,
                                             size_t out_size)
{
    size_t len;
    size_t trim;

    if (!name || !out || out_size == 0) {
        return false;
    }

    len = strlen(name);
    if (len > 4 && !strcmp(name + len - 4, "_end")) {
        trim = 4;
    } else if (len > 4 && !strcmp(name + len - 4, ".end")) {
        trim = 4;
    } else {
        return false;
    }

    if (len - trim + 1 > out_size) {
        return false;
    }
    memcpy(out, name, len - trim);
    out[len - trim] = '\0';
    return true;
}

typedef struct LinxFdtReservedRange {
    hwaddr base;
    hwaddr size;
} LinxFdtReservedRange;

static void linx_fdt_add_memory_range(uint64_t *values, int *entries,
                                      hwaddr base, hwaddr size)
{
    if (!size) {
        return;
    }

    values[(*entries)++] = 2;
    values[(*entries)++] = base;
    values[(*entries)++] = 2;
    values[(*entries)++] = size;
}

static void linx_fdt_set_memory_reg(void *fdt, hwaddr mem_size)
{
    static const LinxFdtReservedRange reserved[] = {
        { LINX_UART_BASE, LINX_UART_SIZE },
        { LINX_VIRT_FINISHER_ADDR, LINX_TEST_FINISHER_SIZE },
        { LINX_VIRTIO_MMIO_BASE, LINX_VIRTIO_MMIO_TOTAL_SIZE },
    };
    uint64_t values[ARRAY_SIZE(reserved) * 4 + 4];
    hwaddr cursor = 0;
    int entries = 0;
    int ret;

    for (size_t i = 0; i < ARRAY_SIZE(reserved); i++) {
        hwaddr hole_base;
        hwaddr hole_end;

        if (!reserved[i].size) {
            continue;
        }

        hole_base = QEMU_ALIGN_DOWN(reserved[i].base,
                                    LINX_MMIO_RESERVED_ALIGN);
        hole_end = QEMU_ALIGN_UP(reserved[i].base + reserved[i].size,
                                 LINX_MMIO_RESERVED_ALIGN);

        if (hole_base >= mem_size) {
            continue;
        }
        if (hole_end <= cursor) {
            continue;
        }
        if (hole_base > cursor) {
            linx_fdt_add_memory_range(values, &entries, cursor,
                                      MIN(hole_base, mem_size) - cursor);
        }

        cursor = MIN(hole_end, mem_size);
        if (cursor >= mem_size) {
            break;
        }
    }

    if (cursor < mem_size) {
        linx_fdt_add_memory_range(values, &entries, cursor,
                                  mem_size - cursor);
    }

    ret = qemu_fdt_setprop_sized_cells_from_array(fdt, "/memory@0", "reg",
                                                  entries / 2, values);
    if (ret < 0) {
        error_report("linx virt: failed to set DT memory reg property");
        exit(1);
    }
}

static void *linx_virt_build_fdt(MachineState *machine,
                                 hwaddr mem_size,
                                 hwaddr initrd_base, hwaddr initrd_size,
                                 int *fdt_alloc_size)
{
    void *fdt;
    char *nodename;
    char *virtio_nodename;
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
     * The `virt` machine maps RAM from address 0 and then places MMIO windows
     * inside that low physical address space. Split the DT "reg" ranges around
     * those windows so Linux does not allocate normal pages from device MMIO.
     */
    linx_fdt_set_memory_reg(fdt, mem_size);

    qemu_fdt_add_subnode(fdt, "/cpus");
    qemu_fdt_setprop_cell(fdt, "/cpus", "#address-cells", 0x1);
    qemu_fdt_setprop_cell(fdt, "/cpus", "#size-cells", 0x0);
    qemu_fdt_setprop_cell(fdt, "/cpus", "timebase-frequency",
                          LINX_TIMEBASE_FREQUENCY);
    qemu_fdt_add_subnode(fdt, "/cpus/cpu@0");
    qemu_fdt_setprop_string(fdt, "/cpus/cpu@0", "device_type", "cpu");
    qemu_fdt_setprop_string(fdt, "/cpus/cpu@0", "compatible", "linx");
    qemu_fdt_setprop_string(fdt, "/cpus/cpu@0", "riscv,isa", "rv64imac");
    qemu_fdt_setprop_string(fdt, "/cpus/cpu@0", "linx,isa", "rv64imac");
    qemu_fdt_setprop_cell(fdt, "/cpus/cpu@0", "reg", 0x0);
    qemu_fdt_setprop_string(fdt, "/cpus/cpu@0", "status", "okay");

    qemu_fdt_add_subnode(fdt, "/cpus/cpu@0/interrupt-controller");
    qemu_fdt_setprop_string(fdt, "/cpus/cpu@0/interrupt-controller",
                            "compatible", "linx,cpu-intc");
    qemu_fdt_setprop(fdt, "/cpus/cpu@0/interrupt-controller",
                     "interrupt-controller", NULL, 0);
    qemu_fdt_setprop_cell(fdt, "/cpus/cpu@0/interrupt-controller",
                          "#interrupt-cells", 0x1);
    qemu_fdt_setprop_cell(fdt, "/cpus/cpu@0/interrupt-controller",
                          "phandle", 0x1);

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

    qemu_fdt_add_subnode(fdt, "/soc/timer");
    qemu_fdt_setprop_string(fdt, "/soc/timer", "compatible",
                            "linx,linx-timer");
    qemu_fdt_setprop(fdt, "/soc/timer", "always-on", NULL, 0);
    qemu_fdt_setprop_cells(fdt, "/soc/timer", "interrupts-extended",
                           0x1, 0x4);

    for (int i = 0; i < LINX_VIRTIO_MMIO_COUNT; i++) {
        const hwaddr base = (hwaddr)LINX_VIRTIO_MMIO_BASE + (hwaddr)i * (hwaddr)LINX_VIRTIO_MMIO_STRIDE;
        const int irq = LINX_VIRTIO_MMIO_IRQ_BASE + i;

        virtio_nodename = g_strdup_printf("/soc/virtio_mmio@%" HWADDR_PRIx, base);
        qemu_fdt_add_subnode(fdt, virtio_nodename);
        qemu_fdt_setprop_string(fdt, virtio_nodename, "compatible", "virtio,mmio");
        qemu_fdt_setprop_cells(fdt, virtio_nodename, "reg",
                               0x0, (uint32_t)base,
                               0x0, (uint32_t)LINX_VIRTIO_MMIO_SIZE);
        qemu_fdt_setprop_cell(fdt, virtio_nodename, "interrupt-parent", 0x1);
        qemu_fdt_setprop_cell(fdt, virtio_nodename, "interrupts", irq);
            virtio_nodename = NULL;
    }

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

    /*
     * 64-bit L.BSTART.* headers encode a 16-bit trailer first, followed by a
     * 32-bit BSTART main word. Recognize them explicitly so relocation
     * fixups do not "repair" valid long-header targets into false positives.
     */
    if ((hw & 0x000f) == 0x000f && addr + 8 <= ram_size) {
        const uint32_t main32 = ldl_le_p(ram + addr + 4);

        if ((main32 & 0x7f) == 0x01 && ((main32 >> 12) & 0x7) != 0) {
            return true;
        }
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

/* Patch a compressed C.BSTART.{DIRECT,COND} relocation (simm12 in bits [15:4]). */
static bool linx_patch_cbstart12_pcrel(uint8_t *ram, size_t ram_size,
                                       hwaddr patch_addr, hwaddr target_addr,
                                       int64_t addend, Error **errp)
{
    uint16_t hw;
    int64_t delta;
    int64_t simm12;
    uint16_t kind;

    if (patch_addr + 2 > ram_size) {
        error_setg(errp, "relocation patch out of RAM bounds @ 0x%" HWADDR_PRIx,
                   patch_addr);
        return false;
    }

    hw = lduw_le_p(ram + patch_addr);
    if (hw & 1) {
        error_setg(errp,
                   "unsupported relocation: expected 16-bit instruction @ 0x%" HWADDR_PRIx,
                   patch_addr);
        return false;
    }

    kind = hw & 0x000f;
    if (kind != 0x0002 && kind != 0x0004) {
        error_setg(errp,
                   "unsupported relocation: not a C.BSTART DIRECT/COND (insn=0x%04x) @ 0x%" HWADDR_PRIx,
                   hw, patch_addr);
        return false;
    }

    delta = (int64_t)(target_addr + addend) - (int64_t)patch_addr;
    if (delta & 1) {
        error_setg(errp,
                   "unaligned c.bstart target: patch @ 0x%" HWADDR_PRIx " -> 0x%" HWADDR_PRIx,
                   patch_addr, target_addr);
        return false;
    }

    simm12 = delta >> 1;
    if (simm12 < -(1 << 11) || simm12 >= (1 << 11)) {
        error_setg(errp,
                   "c.bstart target out of range: patch @ 0x%" HWADDR_PRIx " -> 0x%" HWADDR_PRIx,
                   patch_addr, target_addr);
        return false;
    }

    hw = (uint16_t)((hw & 0x000f) | (((uint16_t)simm12 & 0x0fff) << 4));
    stw_le_p(ram + patch_addr, hw);
    return true;
}

/* Patch a compressed C.SETRET relocation (uimm5 in bits [10:6]). */
static bool linx_patch_csetret5_pcrel(uint8_t *ram, size_t ram_size,
                                      hwaddr patch_addr, hwaddr target_addr,
                                      int64_t addend, Error **errp)
{
    uint16_t hw;
    int64_t delta;
    int64_t uimm5;

    if (patch_addr + 2 > ram_size) {
        error_setg(errp, "relocation patch out of RAM bounds @ 0x%" HWADDR_PRIx,
                   patch_addr);
        return false;
    }

    hw = lduw_le_p(ram + patch_addr);
    if (hw & 1) {
        error_setg(errp,
                   "unsupported relocation: expected 16-bit instruction @ 0x%" HWADDR_PRIx,
                   patch_addr);
        return false;
    }

    /* C.SETRET is encoded as C.MOVI with RegDst==RA and low opcode 0x16. */
    if ((hw & 0x003f) != 0x0016 || (hw & 0x0f80) != 0) {
        error_setg(errp,
                   "unsupported relocation: not a C.SETRET (insn=0x%04x) @ 0x%" HWADDR_PRIx,
                   hw, patch_addr);
        return false;
    }

    delta = (int64_t)(target_addr + addend) - (int64_t)patch_addr;
    if (delta & 1) {
        error_setg(errp,
                   "unaligned c.setret target: patch @ 0x%" HWADDR_PRIx " -> 0x%" HWADDR_PRIx,
                   patch_addr, target_addr);
        return false;
    }

    uimm5 = delta >> 1;
    if (uimm5 < 0 || uimm5 > 31) {
        error_setg(errp,
                   "c.setret target out of range: patch @ 0x%" HWADDR_PRIx " -> 0x%" HWADDR_PRIx,
                   patch_addr, target_addr);
        return false;
    }

    hw = (uint16_t)((hw & ~(0x1fu << 6)) | ((uint16_t)uimm5 << 6));
    stw_le_p(ram + patch_addr, hw);
    return true;
}

/* Patch a 32-bit SETRET relocation (imm20 in bits [31:12], halfword-scaled). */
static bool linx_patch_setret20_pcrel(uint8_t *ram, size_t ram_size,
                                      hwaddr patch_addr, hwaddr target_addr,
                                      int64_t addend, Error **errp)
{
    uint32_t insn;
    int64_t delta;
    int64_t uimm20;

    if (patch_addr + 4 > ram_size) {
        error_setg(errp, "relocation patch out of RAM bounds @ 0x%" HWADDR_PRIx,
                   patch_addr);
        return false;
    }

    insn = ldl_le_p(ram + patch_addr);
    if ((insn & 0x0fffu) != 0x0507u) {
        error_setg(errp,
                   "unsupported relocation: not a SETRET20 instruction (insn=0x%08x) @ 0x%" HWADDR_PRIx,
                   insn, patch_addr);
        return false;
    }

    delta = (int64_t)(target_addr + addend) - (int64_t)patch_addr;
    if (delta & 1) {
        error_setg(errp,
                   "unaligned setret target: patch @ 0x%" HWADDR_PRIx " -> 0x%" HWADDR_PRIx,
                   patch_addr, target_addr);
        return false;
    }

    uimm20 = delta >> 1;
    if (uimm20 < 0 || uimm20 >= (1 << 20)) {
        error_setg(errp,
                   "setret target out of range: patch @ 0x%" HWADDR_PRIx " -> 0x%" HWADDR_PRIx,
                   patch_addr, target_addr);
        return false;
    }

    insn = (insn & 0x0fffu) | ((uint32_t)uimm20 << 12);
    stl_le_p(ram + patch_addr, insn);
    return true;
}

/* Patch an ADDTPC instruction with a PC-relative page offset.
 *
 * ADDTPC encodes a signed imm20 in bits [31:12] which is scaled by 4KiB
 * (imm20 << 12) and added to the current PC page base.
 *
 * Relocation value pairs with signed LO12 consumers, so the HI20 side uses
 * the rounded 4 KiB page of (S + A) rather than the floor page.
 */
static bool linx_patch_addtpc_pcrel(uint8_t *ram, size_t ram_size,
                                    hwaddr patch_addr, hwaddr target_addr,
                                    int64_t addend, Error **errp)
{
    uint32_t insn;
    int64_t delta;
    int64_t simm20;
    int64_t target_page;
    int64_t patch_page;
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

    /*
     * Match the toolchain's signed LO12 pairing: when bit 11 of the low part
     * is set, HI20 must advance by one page so base + sext(lo12) still lands
     * on the final target.
     */
    target_page = (((int64_t)target_addr + addend) + 0x800) & ~0xfffLL;
    patch_page = (int64_t)patch_addr & ~0xfffLL;
    delta = target_page - patch_page;

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
    int64_t delta;
    int64_t hi;
    int64_t lo;
    const uint32_t addiMask = 0x707f;
    const uint32_t addiOpcode = 0x15;
    const uint32_t addiwOpcode = 0x35;
    const uint32_t subiBit = 0x1000;

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

    delta = (int64_t)(target_addr + addend) - (int64_t)patch_addr;
    hi = (delta + 0x800) >> 12;
    lo = delta - (hi << 12);
    if (((insn & addiMask) == addiOpcode ||
         (insn & addiMask) == addiwOpcode) &&
        (delta & 0x800)) {
        insn |= subiBit;
        lo = 0 - lo;
    }

    insn = linx_set_lo12_i(insn, (uint32_t)lo & 0xfff);
    stl_le_p(ram + patch_addr, insn);
    return true;
}

static bool linx_patch_pcrel_lo12_uimm12(uint8_t *ram, size_t ram_size,
                                         hwaddr patch_addr, hwaddr target_addr,
                                         int64_t addend, Error **errp)
{
    uint32_t insn;
    uint32_t lo12;
    const uint32_t addiMask = 0x707f;
    const uint32_t addiOpcode = 0x15;
    const uint32_t addiwOpcode = 0x35;
    const uint32_t subiBit = 0x1000;

    if (patch_addr + 4 > ram_size) {
        error_setg(errp, "PCREL LO12 relocation patch out of RAM bounds @ 0x%" HWADDR_PRIx,
                   patch_addr);
        return false;
    }

    insn = ldl_le_p(ram + patch_addr);
    switch (insn & 0x7f) {
    case 0x15:
    case 0x35:
        break;
    default:
        error_setg(errp,
                   "expected ADDI/ADDIW instruction for PCREL LO12 (insn=0x%08x) @ 0x%" HWADDR_PRIx,
                   insn, patch_addr);
        return false;
    }

    /*
     * ADDTPC pairs with a rounded target page. When the final low 12-bit
     * addend would be negative in the signed page-relative sense, plain ADDI
     * cannot represent it because ADDI uses a zero-extended uimm12. In that
     * case rewrite the consumer to SUBI and encode the positive magnitude,
     * matching the canonical linker split.
     */
    lo12 = (uint32_t)(target_addr + addend) & 0xfff;
    if (((insn & addiMask) == addiOpcode ||
         (insn & addiMask) == addiwOpcode) &&
        (lo12 & 0x800)) {
        insn |= subiBit;
        lo12 = (uint32_t)((0x1000u - lo12) & 0xfff);
    }

    stl_le_p(ram + patch_addr, linx_set_lo12_i(insn, lo12));
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

static bool linx_patch_b_text_pcrel(uint8_t *ram, size_t ram_size,
                                    hwaddr patch_addr, hwaddr target_addr,
                                    int64_t addend, Error **errp)
{
    uint32_t insn;
    int64_t delta;
    int64_t simm25;
    uint32_t imm;

    if (patch_addr + 4 > ram_size) {
        error_setg(errp, "B.TEXT relocation out of bounds @ 0x%" HWADDR_PRIx,
                   patch_addr);
        return false;
    }

    insn = ldl_le_p(ram + patch_addr);
    if ((insn & 0x7f) != 0x03) {
        error_setg(errp,
                   "expected B.TEXT instruction for B.TEXT relocation (insn=0x%08x) @ 0x%" HWADDR_PRIx,
                   insn, patch_addr);
        return false;
    }

    delta = (int64_t)(target_addr + addend) - (int64_t)patch_addr;
    if (delta & 1) {
        error_setg(errp,
                   "unaligned B.TEXT target: patch @ 0x%" HWADDR_PRIx " -> 0x%" HWADDR_PRIx,
                   patch_addr, target_addr);
        return false;
    }

    simm25 = delta >> 1;
    if (simm25 < -(1LL << 24) || simm25 >= (1LL << 24)) {
        error_setg(errp,
                   "B.TEXT target out of range @ 0x%" HWADDR_PRIx " (delta=%" PRId64 ")",
                   patch_addr, delta);
        return false;
    }

    imm = (uint32_t)(simm25 & 0x1ffffffu);
    insn = (insn & 0x7f) | (imm << 7);
    stl_le_p(ram + patch_addr, insn);
    return true;
}

static bool linx_patch_branch_pcrel(uint8_t *ram, size_t ram_size,
                                    hwaddr patch_addr, hwaddr target_addr,
                                    int64_t addend, Error **errp)
{
    uint32_t insn;
    int64_t delta;
    int64_t simm13;
    uint32_t imm12;

    if (patch_addr + 4 > ram_size) {
        error_setg(errp, "BRANCH relocation out of bounds @ 0x%" HWADDR_PRIx,
                   patch_addr);
        return false;
    }

    insn = ldl_le_p(ram + patch_addr);
    delta = (int64_t)(target_addr + addend) - (int64_t)patch_addr;
    if (delta & 1) {
        error_setg(errp,
                   "unaligned BRANCH target: patch @ 0x%" HWADDR_PRIx " -> 0x%" HWADDR_PRIx,
                   patch_addr, target_addr);
        return false;
    }

    simm13 = delta >> 1;
    if (simm13 < -(1 << 12) || simm13 >= (1 << 12)) {
        error_setg(errp,
                   "BRANCH target out of range @ 0x%" HWADDR_PRIx " (delta=%" PRId64 ")",
                   patch_addr, delta);
        return false;
    }

    imm12 = ((uint32_t)(simm13 & 0x0ff) << 25) |
            ((uint32_t)((simm13 >> 8) & 0x1f) << 7);
    insn = (insn & ~((0xffu << 25) | (0x1fu << 7))) | imm12;
    stl_le_p(ram + patch_addr, insn);
    return true;
}

static bool linx_patch_branch22_pcrel(uint8_t *ram, size_t ram_size,
                                      hwaddr patch_addr, hwaddr target_addr,
                                      int64_t addend, Error **errp)
{
    uint32_t insn;
    int64_t delta;
    int64_t simm23;
    uint32_t imm22;

    if (patch_addr + 4 > ram_size) {
        error_setg(errp, "BRANCH_22 relocation out of bounds @ 0x%" HWADDR_PRIx,
                   patch_addr);
        return false;
    }

    insn = ldl_le_p(ram + patch_addr);
    delta = (int64_t)(target_addr + addend) - (int64_t)patch_addr;
    if (delta & 1) {
        error_setg(errp,
                   "unaligned BRANCH_22 target: patch @ 0x%" HWADDR_PRIx " -> 0x%" HWADDR_PRIx,
                   patch_addr, target_addr);
        return false;
    }

    simm23 = delta >> 1;
    if (simm23 < -(1 << 22) || simm23 >= (1 << 22)) {
        error_setg(errp,
                   "BRANCH_22 target out of range @ 0x%" HWADDR_PRIx " (delta=%" PRId64 ")",
                   patch_addr, delta);
        return false;
    }

    imm22 = ((uint32_t)(simm23 & 0x1ffff) << 15) |
            ((uint32_t)((simm23 >> 17) & 0x1f) << 7);
    insn = (insn & ~((0x1ffffu << 15) | (0x1fu << 7))) | imm22;
    stl_le_p(ram + patch_addr, insn);
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
    if (opcode == 0x03) {
        /* B.TEXT: PC-relative decoupled body pointer (simm25 in halfwords). */
        return linx_patch_b_text_pcrel(ram, ram_size, patch_addr, target_addr,
                                       addend, errp);
    } else if (opcode == 0x07) {
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

static bool linx_reloc_uses_instruction_fallback(uint32_t rtype)
{
    switch (rtype) {
    case R_LINX_B17_PCREL:
    case R_LINX_B17_PLT:
    case R_LINX_B25_PCREL:
    case R_LINX_PCR17_LOAD:
    case R_LINX_PCR17_STORE:
        return true;
    default:
        return false;
    }
}

#if TARGET_LONG_BITS == 32
static void linx_collect_body_ranges_elf32(const uint8_t *buf, size_t len,
                                           const Elf32_Shdr *strtab_sh,
                                           const Elf32_Sym *syms,
                                           size_t nsyms,
                                           const hwaddr *sym_addr,
                                           CPULinxState *env)
{
    LinxBodyRange *ranges;
    uint32_t count = 0;
    size_t i;

    if (!env) {
        return;
    }

    for (i = 0; i < nsyms; i++) {
        const Elf32_Sym *end_sym = &syms[i];
        char start_name[256];
        size_t j;

        if (end_sym->st_shndx == SHN_UNDEF || end_sym->st_name >= strtab_sh->sh_size) {
            continue;
        }
        if (!linx_body_end_name_to_start_name(
                linx_elf32_sym_name(buf, len, strtab_sh, end_sym),
                start_name, sizeof(start_name))) {
            continue;
        }
        for (j = 0; j < nsyms; j++) {
            const Elf32_Sym *start_sym = &syms[j];
            if (start_sym->st_shndx == SHN_UNDEF ||
                start_sym->st_name >= strtab_sh->sh_size) {
                continue;
            }
            if (strcmp(linx_elf32_sym_name(buf, len, strtab_sh, start_sym),
                       start_name) != 0) {
                continue;
            }
            if (sym_addr[j] < sym_addr[i]) {
                count++;
            }
            break;
        }
    }

    if (count == 0) {
        linx_set_body_ranges(env, NULL, 0);
        return;
    }

    ranges = g_new0(LinxBodyRange, count);
    count = 0;
    for (i = 0; i < nsyms; i++) {
        const Elf32_Sym *end_sym = &syms[i];
        char start_name[256];
        size_t j;

        if (end_sym->st_shndx == SHN_UNDEF || end_sym->st_name >= strtab_sh->sh_size) {
            continue;
        }
        if (!linx_body_end_name_to_start_name(
                linx_elf32_sym_name(buf, len, strtab_sh, end_sym),
                start_name, sizeof(start_name))) {
            continue;
        }
        for (j = 0; j < nsyms; j++) {
            const Elf32_Sym *start_sym = &syms[j];
            if (start_sym->st_shndx == SHN_UNDEF ||
                start_sym->st_name >= strtab_sh->sh_size) {
                continue;
            }
            if (strcmp(linx_elf32_sym_name(buf, len, strtab_sh, start_sym),
                       start_name) != 0) {
                continue;
            }
            if (sym_addr[j] < sym_addr[i]) {
                ranges[count].start = sym_addr[j];
                ranges[count].end = sym_addr[i];
                count++;
            }
            break;
        }
    }

    linx_set_body_ranges(env, ranges, count);
}

static bool linx_load_elf32_rel(const uint8_t *buf, size_t len,
                                uint8_t *ram, size_t ram_size,
                                CPULinxState *env,
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
    uint64_t *call_continuations = NULL;
    uint32_t call_continuation_count = 0;
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

            if (rtype == R_LINX_NONE) {
                continue;
            }

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
            } else if (rtype == R_LINX_CBSTART12_PCREL) {
                if (!linx_patch_cbstart12_pcrel(ram, ram_size, patch_addr, target,
                                                (int64_t)rela[j].r_addend, errp)) {
                    goto fail;
                }
                continue;
            } else if (rtype == R_LINX_CSETRET5_PCREL) {
                if (!linx_patch_csetret5_pcrel(ram, ram_size, patch_addr, target,
                                               (int64_t)rela[j].r_addend, errp)) {
                    goto fail;
                }
                if (!linx_record_call_continuation(&call_continuations,
                                                   &call_continuation_count,
                                                   (uint64_t)((int64_t)target +
                                                              (int64_t)rela[j].r_addend),
                                                   errp)) {
                    goto fail;
                }
                continue;
            } else if (rtype == R_LINX_SETRET20_PCREL) {
                if (!linx_patch_setret20_pcrel(ram, ram_size, patch_addr, target,
                                               (int64_t)rela[j].r_addend, errp)) {
                    goto fail;
                }
                if (!linx_record_call_continuation(&call_continuations,
                                                   &call_continuation_count,
                                                   (uint64_t)((int64_t)target +
                                                              (int64_t)rela[j].r_addend),
                                                   errp)) {
                    goto fail;
                }
                continue;
            } else if (rtype == R_LINX_B12_PCREL) {
                if (!linx_patch_branch_pcrel(ram, ram_size, patch_addr, target,
                                             (int64_t)rela[j].r_addend, errp)) {
                    goto fail;
                }
                continue;
            } else if (rtype == R_LINX_J22_PCREL) {
                if (!linx_patch_branch22_pcrel(ram, ram_size, patch_addr, target,
                                               (int64_t)rela[j].r_addend, errp)) {
                    goto fail;
                }
                continue;
            } else if (rtype == R_LINX_PCREL_HI20) {
                if (!linx_patch_addtpc_pcrel(ram, ram_size, patch_addr, target,
                                             (int64_t)rela[j].r_addend, errp)) {
                    goto fail;
                }
                continue;
            } else if (rtype == R_LINX_LO12) {
                if (!linx_patch_pcrel_lo12_uimm12(
                        ram, ram_size, patch_addr, target,
                        (int64_t)rela[j].r_addend, errp)) {
                    goto fail;
                }
                continue;
            }
            
            if (!linx_reloc_uses_instruction_fallback(rtype)) {
                error_setg(errp, "unsupported LinxISA ET_REL relocation type %u",
                           rtype);
                goto fail;
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

    linx_collect_body_ranges_elf32(buf, len, strtab_sh, syms, nsyms, sym_addr,
                                   env);
    linx_set_call_continuations(env, call_continuations,
                                call_continuation_count);
    *image_end = end;
    g_free(sym_addr);
    g_free(sec_addr);
    return true;

fail:
    g_free(call_continuations);
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
    bool use_phys_layout = false;

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
        if (ph->p_paddr != 0 || (hwaddr)ph->p_vaddr != (hwaddr)ph->p_paddr) {
            use_phys_layout = true;
        }
        addr = use_phys_layout ? (hwaddr)ph->p_paddr : (hwaddr)ph->p_vaddr;
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

        seg_addr = use_phys_layout ? (hwaddr)ph->p_paddr : (hwaddr)ph->p_vaddr;
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
    if (use_phys_layout) {
        bool found_entry = false;
        for (i = 0; i < eh->e_phnum; i++) {
            const Elf32_Phdr *ph = &phdrs[i];
            hwaddr virt_start, virt_end, phys_start;

            if (ph->p_type != PT_LOAD || ph->p_memsz == 0) {
                continue;
            }
            virt_start = (hwaddr)ph->p_vaddr;
            virt_end = virt_start + (hwaddr)ph->p_memsz;
            if ((hwaddr)eh->e_entry < virt_start || (hwaddr)eh->e_entry >= virt_end) {
                continue;
            }
            phys_start = (hwaddr)ph->p_paddr + bias;
            entry_addr = phys_start + ((hwaddr)eh->e_entry - virt_start);
            found_entry = true;
            break;
        }
        if (!found_entry) {
            error_setg(errp, "entry point does not map to a PT_LOAD segment");
            return false;
        }
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
        seg_addr = use_phys_layout ? (hwaddr)ph->p_paddr : (hwaddr)ph->p_vaddr;
        seg_start = seg_addr + bias;
        seg_end = seg_start + (hwaddr)ph->p_memsz;
        if ((!use_phys_layout && entry_addr >= seg_start && entry_addr < seg_end) ||
            (use_phys_layout &&
             (hwaddr)eh->e_entry >= (hwaddr)ph->p_vaddr &&
             (hwaddr)eh->e_entry < (hwaddr)ph->p_vaddr + (hwaddr)ph->p_memsz)) {
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
static void linx_collect_body_ranges_elf64(const uint8_t *buf, size_t len,
                                           const Elf64_Shdr *strtab_sh,
                                           const Elf64_Sym *syms,
                                           size_t nsyms,
                                           const hwaddr *sym_addr,
                                           CPULinxState *env)
{
    LinxBodyRange *ranges;
    uint32_t count = 0;
    size_t i;

    if (!env) {
        return;
    }

    for (i = 0; i < nsyms; i++) {
        const Elf64_Sym *end_sym = &syms[i];
        char start_name[256];
        size_t j;

        if (end_sym->st_shndx == SHN_UNDEF || end_sym->st_name >= strtab_sh->sh_size) {
            continue;
        }
        if (!linx_body_end_name_to_start_name(
                linx_elf64_sym_name(buf, len, strtab_sh, end_sym),
                start_name, sizeof(start_name))) {
            continue;
        }
        for (j = 0; j < nsyms; j++) {
            const Elf64_Sym *start_sym = &syms[j];
            if (start_sym->st_shndx == SHN_UNDEF ||
                start_sym->st_name >= strtab_sh->sh_size) {
                continue;
            }
            if (strcmp(linx_elf64_sym_name(buf, len, strtab_sh, start_sym),
                       start_name) != 0) {
                continue;
            }
            if (sym_addr[j] < sym_addr[i]) {
                count++;
            }
            break;
        }
    }

    if (count == 0) {
        linx_set_body_ranges(env, NULL, 0);
        return;
    }

    ranges = g_new0(LinxBodyRange, count);
    count = 0;
    for (i = 0; i < nsyms; i++) {
        const Elf64_Sym *end_sym = &syms[i];
        char start_name[256];
        size_t j;

        if (end_sym->st_shndx == SHN_UNDEF || end_sym->st_name >= strtab_sh->sh_size) {
            continue;
        }
        if (!linx_body_end_name_to_start_name(
                linx_elf64_sym_name(buf, len, strtab_sh, end_sym),
                start_name, sizeof(start_name))) {
            continue;
        }
        for (j = 0; j < nsyms; j++) {
            const Elf64_Sym *start_sym = &syms[j];
            if (start_sym->st_shndx == SHN_UNDEF ||
                start_sym->st_name >= strtab_sh->sh_size) {
                continue;
            }
            if (strcmp(linx_elf64_sym_name(buf, len, strtab_sh, start_sym),
                       start_name) != 0) {
                continue;
            }
            if (sym_addr[j] < sym_addr[i]) {
                ranges[count].start = sym_addr[j];
                ranges[count].end = sym_addr[i];
                count++;
            }
            break;
        }
    }

    linx_set_body_ranges(env, ranges, count);
}

static bool linx_load_elf64_rel(const uint8_t *buf, size_t len,
                                uint8_t *ram, size_t ram_size,
                                CPULinxState *env,
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
    uint64_t *call_continuations = NULL;
    uint32_t call_continuation_count = 0;
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

            if (rtype == R_LINX_NONE) {
                continue;
            }

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
            } else if (rtype == R_LINX_CBSTART12_PCREL) {
                if (!linx_patch_cbstart12_pcrel(ram, ram_size, patch_addr, target,
                                                rela[j].r_addend, errp)) {
                    goto fail;
                }
                continue;
            } else if (rtype == R_LINX_CSETRET5_PCREL) {
                if (!linx_patch_csetret5_pcrel(ram, ram_size, patch_addr, target,
                                               rela[j].r_addend, errp)) {
                    goto fail;
                }
                if (!linx_record_call_continuation(&call_continuations,
                                                   &call_continuation_count,
                                                   (uint64_t)((int64_t)target +
                                                              rela[j].r_addend),
                                                   errp)) {
                    goto fail;
                }
                continue;
            } else if (rtype == R_LINX_SETRET20_PCREL) {
                if (!linx_patch_setret20_pcrel(ram, ram_size, patch_addr, target,
                                               rela[j].r_addend, errp)) {
                    goto fail;
                }
                if (!linx_record_call_continuation(&call_continuations,
                                                   &call_continuation_count,
                                                   (uint64_t)((int64_t)target +
                                                              rela[j].r_addend),
                                                   errp)) {
                    goto fail;
                }
                continue;
            } else if (rtype == R_LINX_B12_PCREL) {
                if (!linx_patch_branch_pcrel(ram, ram_size, patch_addr, target,
                                             rela[j].r_addend, errp)) {
                    goto fail;
                }
                continue;
            } else if (rtype == R_LINX_J22_PCREL) {
                if (!linx_patch_branch22_pcrel(ram, ram_size, patch_addr, target,
                                               rela[j].r_addend, errp)) {
                    goto fail;
                }
                continue;
            } else if (rtype == R_LINX_PCREL_HI20) {
                if (!linx_patch_addtpc_pcrel(ram, ram_size, patch_addr, target,
                                             rela[j].r_addend, errp)) {
                    goto fail;
                }
                continue;
            } else if (rtype == R_LINX_LO12) {
                if (!linx_patch_pcrel_lo12_uimm12(
                        ram, ram_size, patch_addr, target,
                        rela[j].r_addend, errp)) {
                    goto fail;
                }
                continue;
            }
            
            if (!linx_reloc_uses_instruction_fallback(rtype)) {
                error_setg(errp, "unsupported LinxISA ET_REL relocation type %u",
                           rtype);
                goto fail;
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

    linx_collect_body_ranges_elf64(buf, len, strtab_sh, syms, nsyms, sym_addr,
                                   env);
    linx_set_call_continuations(env, call_continuations,
                                call_continuation_count);
    *image_end = end;
    g_free(sym_addr);
    g_free(sec_addr);
    return true;

fail:
    g_free(call_continuations);
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
    bool use_phys_layout = false;

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
        if (ph->p_paddr != 0 || (hwaddr)ph->p_vaddr != (hwaddr)ph->p_paddr) {
            use_phys_layout = true;
        }
        addr = use_phys_layout ? (hwaddr)ph->p_paddr : (hwaddr)ph->p_vaddr;
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

        seg_addr = use_phys_layout ? (hwaddr)ph->p_paddr : (hwaddr)ph->p_vaddr;
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
    if (use_phys_layout) {
        bool found_entry = false;
        for (i = 0; i < eh->e_phnum; i++) {
            const Elf64_Phdr *ph = &phdrs[i];
            hwaddr virt_start, virt_end, phys_start;

            if (ph->p_type != PT_LOAD || ph->p_memsz == 0) {
                continue;
            }
            virt_start = (hwaddr)ph->p_vaddr;
            virt_end = virt_start + (hwaddr)ph->p_memsz;
            if ((hwaddr)eh->e_entry < virt_start || (hwaddr)eh->e_entry >= virt_end) {
                continue;
            }
            phys_start = (hwaddr)ph->p_paddr + bias;
            entry_addr = phys_start + ((hwaddr)eh->e_entry - virt_start);
            found_entry = true;
            break;
        }
        if (!found_entry) {
            error_setg(errp, "entry point does not map to a PT_LOAD segment");
            return false;
        }
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
        seg_addr = use_phys_layout ? (hwaddr)ph->p_paddr : (hwaddr)ph->p_vaddr;
        seg_start = seg_addr + bias;
        seg_end = seg_start + (hwaddr)ph->p_memsz;
        if ((!use_phys_layout && entry_addr >= seg_start && entry_addr < seg_end) ||
            (use_phys_layout &&
             (hwaddr)eh->e_entry >= (hwaddr)ph->p_vaddr &&
             (hwaddr)eh->e_entry < (hwaddr)ph->p_vaddr + (hwaddr)ph->p_memsz)) {
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
                          CPULinxState *env,
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
        ok = linx_load_elf32_rel(buf, len, ram, ram_size, env, load_base,
                                 entry, image_end, errp);
    } else {
        linx_set_body_ranges(env, NULL, 0);
        ok = linx_load_elf32_exec(buf, len, ram, ram_size, load_base,
                                  entry, image_end, errp);
    }
#else
    if (len >= sizeof(Elf64_Ehdr) && ((const Elf64_Ehdr *)buf)->e_type == ET_REL) {
        ok = linx_load_elf64_rel(buf, len, ram, ram_size, env, load_base,
                                 entry, image_end, errp);
    } else {
        linx_set_body_ranges(env, NULL, 0);
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

    if (!s->entry_valid) {
        error_report("linx virt: invalid entry point");
        exit(1);
    }

    trace_linx_virt_reset(s->entry, s->initial_sp, s->exit_trampoline);

    // Quiet boot - don't print entry address to avoid mixing with UART output
    // qemu_log_mask(LOG_TRACE, "linx virt: entry=0x%" HWADDR_PRIx " sp=0x%" HWADDR_PRIx "\n",
    //               s->entry, s->initial_sp);

    for (unsigned pe = 0; pe < s->pe_count; pe++) {
        CPULinxState *env = &s->cpu[pe]->env;

        cpu_reset(CPU(s->cpu[pe]));
        env->pc = s->entry;
        env->gpr[LINX_REG_SP] = s->initial_sp - pe * 0x10000;
        env->gpr[LINX_REG_RA] = s->exit_trampoline;
        env->gpr[LINX_REG_ZERO] = 0;
        env->gpr[LINX_REG_A0] = pe;
        env->gpr[LINX_REG_A1] = s->fdt_addr;
        env->gpr[LINX_REG_A2] = 0;
        env->pe_id = pe;
    }
}

static uint32_t linx_memwatch_pack_attrs(MemTxAttrs attrs)
{
    uint32_t v = 0;

    if (attrs.secure) {
        v |= 1u << 0;
    }
    if (attrs.user) {
        v |= 1u << 1;
    }
    if (attrs.debug) {
        v |= 1u << 2;
    }
    if (attrs.memory) {
        v |= 1u << 3;
    }
    if (attrs.unspecified) {
        v |= 1u << 4;
    }
    v |= (uint32_t)attrs.requester_id << 16;
    return v;
}

static uint64_t linx_memwatch_read(void *opaque, hwaddr addr, unsigned size)
{
    LinxVirtMachineState *s = opaque;
    const hwaddr paddr = (hwaddr)s->dfx_memwatch_addr + addr;
    uint64_t v = 0;

    if (!s->dfx_ram_ptr || paddr + size > s->parent_obj.ram_size) {
        return 0;
    }

    switch (size) {
    case 1:
        v = *(uint8_t *)(s->dfx_ram_ptr + paddr);
        break;
    case 2:
        v = lduw_le_p(s->dfx_ram_ptr + paddr);
        break;
    case 4:
        v = ldl_le_p(s->dfx_ram_ptr + paddr);
        break;
    case 8:
        v = ldq_le_p(s->dfx_ram_ptr + paddr);
        break;
    default:
        memcpy(&v, s->dfx_ram_ptr + paddr, MIN((unsigned)sizeof(v), size));
        break;
    }

    return v;
}

static void linx_memwatch_do_write(LinxVirtMachineState *s, hwaddr addr,
                                   uint64_t data, unsigned size,
                                   MemTxAttrs attrs)
{
    const hwaddr paddr = (hwaddr)s->dfx_memwatch_addr + addr;
    uint64_t old = 0;
    uint64_t pc = 0;
    uint64_t sp = 0;
    uint64_t ra = 0;
    uint64_t a0 = 0;
    int cpu = 0;
    uint32_t attrs_packed = linx_memwatch_pack_attrs(attrs);

    if (!s->dfx_ram_ptr || paddr + size > s->parent_obj.ram_size) {
        return;
    }

    if (current_cpu) {
        cpu = 1;
        LinxCPU *lc = LINX_CPU(current_cpu);
        pc = lc->env.pc;
        sp = lc->env.gpr[LINX_REG_SP];
        ra = lc->env.gpr[LINX_REG_RA];
        a0 = lc->env.gpr[LINX_REG_A0];
    }

    /* Read old value (little-endian) for tracing. */
    old = linx_memwatch_read(s, addr, size);

    /* Store new bytes into backing RAM (little-endian). */
    switch (size) {
    case 1:
        *(uint8_t *)(s->dfx_ram_ptr + paddr) = (uint8_t)data;
        break;
    case 2:
        stw_le_p(s->dfx_ram_ptr + paddr, (uint16_t)data);
        break;
    case 4:
        stl_le_p(s->dfx_ram_ptr + paddr, (uint32_t)data);
        break;
    case 8:
        stq_le_p(s->dfx_ram_ptr + paddr, data);
        break;
    default:
        memcpy(s->dfx_ram_ptr + paddr, &data, MIN((unsigned)sizeof(data), size));
        break;
    }

    trace_linx_virt_memwatch_write(paddr, size, old, data, pc, sp, ra, a0,
                                   cpu, attrs_packed);
    if (s->dfx_memwatch_stop) {
        qemu_system_debug_request();
    }
}

static void linx_memwatch_write(void *opaque, hwaddr addr, uint64_t data,
                                unsigned size)
{
    linx_memwatch_do_write(opaque, addr, data, size, MEMTXATTRS_UNSPECIFIED);
}

static MemTxResult linx_memwatch_read_with_attrs(void *opaque, hwaddr addr,
                                                 uint64_t *data, unsigned size,
                                                 MemTxAttrs attrs)
{
    (void)attrs;
    *data = linx_memwatch_read(opaque, addr, size);
    return MEMTX_OK;
}

static MemTxResult linx_memwatch_write_with_attrs(void *opaque, hwaddr addr,
                                                  uint64_t data, unsigned size,
                                                  MemTxAttrs attrs)
{
    linx_memwatch_do_write(opaque, addr, data, size, attrs);
    return MEMTX_OK;
}

static const MemoryRegionOps linx_memwatch_ops = {
    .read = linx_memwatch_read,
    .write = linx_memwatch_write,
    .read_with_attrs = linx_memwatch_read_with_attrs,
    .write_with_attrs = linx_memwatch_write_with_attrs,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
        .unaligned = true,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
        .unaligned = true,
    },
};

static bool linx_virt_get_dfx_memwatch_stop(Object *obj, Error **errp)
{
    LinxVirtMachineState *s = LINX_VIRT_MACHINE(obj);
    (void)errp;
    return s->dfx_memwatch_stop;
}

static void linx_virt_set_dfx_memwatch_stop(Object *obj, bool value,
                                            Error **errp)
{
    LinxVirtMachineState *s = LINX_VIRT_MACHINE(obj);
    (void)errp;
    s->dfx_memwatch_stop = value;
}

static char *linx_virt_get_cross_model_dump(Object *obj, Error **errp)
{
    LinxVirtMachineState *s = LINX_VIRT_MACHINE(obj);
    (void)errp;
    return g_strdup(s->cross_model_dump);
}

static void linx_virt_set_cross_model_dump(Object *obj, const char *value,
                                           Error **errp)
{
    LinxVirtMachineState *s = LINX_VIRT_MACHINE(obj);
    (void)errp;
    g_free(s->cross_model_dump);
    s->cross_model_dump = g_strdup(value);
}

static void linx_cross_model_shutdown(Notifier *notifier, void *opaque)
{
    LinxVirtMachineState *s = container_of(notifier, LinxVirtMachineState,
                                           cross_model_shutdown_notifier);
    g_autofree char *tmp_path = NULL;
    g_autoptr(GError) err = NULL;
    ShutdownCause cause = *(ShutdownCause *)opaque;

    if (!s->cross_model_dump_pending || !s->cross_model_dump ||
        cause != SHUTDOWN_CAUSE_GUEST_SHUTDOWN) {
        return;
    }

    tmp_path = g_strdup_printf("%s.tmp", s->cross_model_dump);
    if (!g_file_set_contents(tmp_path,
                             (const char *)s->dfx_ram_ptr +
                                 s->cross_model_address,
                             s->cross_model_size, &err) ||
        g_rename(tmp_path, s->cross_model_dump) != 0) {
        error_report("linx virt: cannot write cross-model dump '%s': %s",
                     s->cross_model_dump,
                     err ? err->message : strerror(errno));
        unlink(tmp_path);
    }
}

static void linx_virt_instance_init(Object *obj)
{
    LinxVirtMachineState *s = LINX_VIRT_MACHINE(obj);

    s->dfx_memwatch_addr = 0;
    s->dfx_memwatch_len = 0;
    s->dfx_memwatch_stop = false;
    s->dfx_ram_ptr = NULL;
    s->cross_model_dump = NULL;
    s->cross_model_address = 0;
    s->cross_model_size = 0;
    s->cross_model_dump_pending = false;

    object_property_add_uint64_ptr(obj, "dfx-memwatch-addr",
                                   &s->dfx_memwatch_addr,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "dfx-memwatch-addr",
                                    "Physical base address of RAM memwatch overlay");

    object_property_add_uint32_ptr(obj, "dfx-memwatch-len",
                                   &s->dfx_memwatch_len,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "dfx-memwatch-len",
                                    "Length in bytes of RAM memwatch overlay (0 disables)");

    object_property_add_bool(obj, "dfx-memwatch-stop",
                             linx_virt_get_dfx_memwatch_stop,
                             linx_virt_set_dfx_memwatch_stop);
    object_property_set_description(obj, "dfx-memwatch-stop",
                                    "Stop VM (RUN_STATE_DEBUG) on memwatch write");

    object_property_add_str(obj, "cross-model-dump",
                            linx_virt_get_cross_model_dump,
                            linx_virt_set_cross_model_dump);
    object_property_set_description(obj, "cross-model-dump",
                                    "Write a guest RAM range after test-finisher shutdown");

    object_property_add_uint64_ptr(obj, "cross-model-address",
                                   &s->cross_model_address,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "cross-model-address",
                                    "Physical start address of the cross-model result");

    object_property_add_uint32_ptr(obj, "cross-model-size",
                                   &s->cross_model_size,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_set_description(obj, "cross-model-size",
                                    "Size in bytes of the cross-model result");
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
    const hwaddr fdt_gap = 0x10000;

    if (machine->smp.cpus != 1 && machine->smp.cpus != 4) {
        error_report("linx virt: DavinciOO supports either 1 PE or Core4 (-smp 4)");
        exit(1);
    }
    s->pe_count = machine->smp.cpus;

    hwaddr load_base = 0x10000;
    hwaddr tramp;
    hwaddr sp;

    if (!machine->kernel_filename) {
        error_report("linx virt: missing -kernel <linxisa kernel image>");
        exit(1);
    }

    trace_linx_virt_load_kernel(machine->kernel_filename);

    if (!machine->ram) {
        error_report("linx virt: machine RAM not initialized");
        exit(1);
    }

    memory_region_add_subregion(get_system_memory(), 0, machine->ram);
    s->dfx_ram_ptr = memory_region_get_ram_ptr(machine->ram);

    if ((s->cross_model_dump != NULL) != (s->cross_model_size != 0)) {
        error_report("linx virt: cross-model-dump and cross-model-size must be specified together");
        exit(1);
    }
    if (s->cross_model_dump) {
        uint64_t end = s->cross_model_address + s->cross_model_size;
        if (s->cross_model_size == 0 ||
            s->cross_model_size > LINX_CROSS_MODEL_DUMP_MAX ||
            end < s->cross_model_address || end > machine->ram_size) {
            error_report("linx virt: invalid cross-model RAM range: "
                         "addr=0x%" PRIx64 " size=%u ram=0x%" PRIx64,
                         s->cross_model_address, s->cross_model_size,
                         (uint64_t)machine->ram_size);
            exit(1);
        }
        s->cross_model_shutdown_notifier.notify = linx_cross_model_shutdown;
        qemu_register_shutdown_notifier(&s->cross_model_shutdown_notifier);
    }

    if (s->dfx_memwatch_len) {
        hwaddr end = (hwaddr)s->dfx_memwatch_addr +
                     (hwaddr)s->dfx_memwatch_len;
        if (end < (hwaddr)s->dfx_memwatch_addr ||
            end > machine->ram_size) {
            error_report("linx virt: memwatch region out of RAM bounds: "
                         "addr=0x%" PRIx64 " len=%u ram=0x%" PRIx64,
                         s->dfx_memwatch_addr, s->dfx_memwatch_len,
                         (uint64_t)machine->ram_size);
            exit(1);
        }

        memory_region_init_io(&s->dfx_memwatch, OBJECT(machine),
                              &linx_memwatch_ops, s, "linx.memwatch",
                              s->dfx_memwatch_len);
        memory_region_add_subregion_overlap(get_system_memory(),
                                            (hwaddr)s->dfx_memwatch_addr,
                                            &s->dfx_memwatch, 1);
        trace_linx_virt_memwatch_enable(s->dfx_memwatch_addr,
                                        s->dfx_memwatch_len,
                                        s->dfx_memwatch_stop ? 1 : 0);
    }

    for (unsigned pe = 0; pe < s->pe_count; pe++) {
        s->cpu[pe] = LINX_CPU(cpu_create(machine->cpu_type));
        s->cpu[pe]->boot_pe_id = pe;
    }

    /* Initialize UART */
    s->uart.cross_model_dump_pending = &s->cross_model_dump_pending;
    linx_uart_init(&s->uart, s->cpu[0]);

    /*
     * Provide one virtio-mmio transport for bring-up disk boot experiments.
     *
     * Guests can bind a backend with e.g.:
     *   -drive if=none,id=vd0,file=<img>,format=raw
     *   -device virtio-blk-device,drive=vd0
     */
    for (int i = 0; i < LINX_VIRTIO_MMIO_COUNT; i++) {
        const hwaddr base = (hwaddr)LINX_VIRTIO_MMIO_BASE + (hwaddr)i * (hwaddr)LINX_VIRTIO_MMIO_STRIDE;
        const int irq = LINX_VIRTIO_MMIO_IRQ_BASE + i;
        DeviceState *vdev = qdev_new(TYPE_VIRTIO_MMIO);
        SysBusDevice *sbd = SYS_BUS_DEVICE(vdev);

        /* Use modern virtio-mmio transport by default for Linx guests. */
        qdev_prop_set_bit(vdev, "force-legacy", false);
        qdev_prop_set_bit(vdev, "ioeventfd", false);
        qdev_prop_set_bit(vdev, "format_transport_address", true);

        sysbus_realize_and_unref(sbd, &error_fatal);
        sysbus_mmio_map(sbd, 0, base);
        s->virtio_irq[i] = qemu_allocate_irq(linx_virt_set_irq, s->cpu[0], irq);
        sysbus_connect_irq(sbd, 0, s->virtio_irq[i]);
        if (linx_virtio_mmio_debug_enabled()) {
            fprintf(stderr,
                    "LINX_VIRTIO_MMIO_SLOT index=%d base=0x%" HWADDR_PRIx
                    " size=0x%x irq=%d proxy=%p irq_handle=%p\n",
                    i, base, LINX_VIRTIO_MMIO_SIZE, irq, vdev,
                    s->virtio_irq[i]);
        }
    }

    ram = s->dfx_ram_ptr;
    for (unsigned pe = 0; pe < s->pe_count; pe++) {
        hwaddr pe_entry = 0;
        hwaddr pe_image_end = 0;

        if (!linx_load_elf(machine->kernel_filename, ram, machine->ram_size,
                           &s->cpu[pe]->env, load_base, &pe_entry,
                           &pe_image_end, &error_fatal)) {
            exit(1);
        }
        if (pe == 0) {
            entry = pe_entry;
            image_end = pe_image_end;
        } else if (pe_entry != entry || pe_image_end != image_end) {
            error_report("linx virt: inconsistent Core4 ELF metadata for PE%u",
                         pe);
            exit(1);
        }
    }

    trace_linx_virt_loaded_elf(entry, image_end);

    /*
     * Linux reserves the loaded image through PMD-aligned _end when strict
     * kernel RWX is enabled. Keep initrd/FDT payloads outside that range.
     */
    cur = linx_align_up(image_end, LINX_LINUX_PMD_ALIGN);

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

        trace_linx_virt_loaded_initrd(machine->initrd_filename, initrd_base, initrd_size);
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
    fdt_size = fdt_totalsize(fdt);
    if (fdt_size <= 0) {
        error_report("linx virt: invalid packed device tree");
        exit(1);
    }

    tramp = (machine->ram_size - 8) & ~0xfULL;
    sp = (tramp - 0x10000) & ~0xfULL;
    if (sp <= (hwaddr)fdt_size + fdt_gap) {
        error_report("linx virt: FDT does not fit in RAM");
        exit(1);
    }
    fdt_addr = (sp - (hwaddr)fdt_size - fdt_gap) & ~0xfULL;
    if (fdt_addr < cur || (size_t)fdt_addr + (size_t)fdt_size > machine->ram_size) {
        error_report("linx virt: FDT placement overlaps payload (fdt=0x%" HWADDR_PRIx
                     " payload_end=0x%" HWADDR_PRIx " size=0x%x)",
                     fdt_addr, cur, fdt_size);
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

    trace_linx_virt_fdt(fdt_addr, (uint32_t)fdt_size);

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
    s->entry_valid = true;
    s->initial_sp = sp;
    s->exit_trampoline = tramp;
    s->fdt_addr = fdt_addr;

    for (unsigned pe = 0; pe < s->pe_count; pe++) {
        s->cpu[pe]->boot_pc = entry;
        s->cpu[pe]->boot_sp = sp - pe * 0x10000;
        s->cpu[pe]->boot_ra = tramp;
        s->cpu[pe]->boot_a0 = pe;
        s->cpu[pe]->boot_a1 = fdt_addr;
        s->cpu[pe]->boot_a2 = 0;
    }

    qemu_register_reset(linx_virt_reset, s);
}

static void linx_virt_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "QEMU LinxISA Virtual Machine";
    mc->init = linx_virt_init;
    mc->default_cpu_type = TYPE_LINX_CPU_LINX;
    mc->default_cpus = 1;
    mc->max_cpus = 4;
    mc->default_ram_id = "linx.virt.ram";
    mc->default_ram_size = 128 * MiB;
}

static const TypeInfo linx_virt_machine_info = {
    .name = TYPE_LINX_VIRT_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(LinxVirtMachineState),
    .class_init = linx_virt_machine_class_init,
    .instance_init = linx_virt_instance_init,
};

static void linx_virt_machine_register_types(void)
{
    type_register_static(&linx_virt_machine_info);
}

type_init(linx_virt_machine_register_types)
