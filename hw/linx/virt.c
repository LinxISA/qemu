/*
 * QEMU LinxISA "virt" machine
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/core/boards.h"
#include "system/address-spaces.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "elf.h"
#include "chardev/char.h"
#include "qemu/qemu-print.h"

#include "cpu.h"

static bool linx_virt_debug_enabled(void)
{
    const char *v = getenv("LINX_VIRT_DEBUG");
    return v && v[0] && strcmp(v, "0") != 0;
}

/* UART base address */
#define LINX_UART_BASE 0x10000000
#define LINX_UART_SIZE 0x100

/* Exit register address */
#define LINX_EXIT_REG 0x10000004

/* Simple UART state */
typedef struct LinxUARTState {
    MemoryRegion mmio;
    QemuMutex lock;
} LinxUARTState;

static uint64_t linx_uart_read(void *opaque, hwaddr addr, unsigned size)
{
    (void)opaque;
    (void)size;

    if (addr == 0) {
        /* UART data register - return 0 */
        return 0;
    } else if (addr >= 4 && addr < 8) {
        /* Status register - return ready */
        return 1;  /* TX ready */
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
    /* Output to stdout only */
    fputc(c, stdout);
    fflush(stdout);
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

static void linx_uart_init(LinxUARTState *s)
{
    qemu_mutex_init(&s->lock);
    memory_region_init_io(&s->mmio, NULL, &linx_uart_ops, s,
                          "linx-uart", LINX_UART_SIZE);
    memory_region_add_subregion(get_system_memory(), LINX_UART_BASE,
                                &s->mmio);
}

#define TYPE_LINX_VIRT_MACHINE MACHINE_TYPE_NAME("virt")
OBJECT_DECLARE_SIMPLE_TYPE(LinxVirtMachineState, LINX_VIRT_MACHINE)

typedef struct LinxVirtMachineState {
    MachineState parent_obj;

    LinxCPU *cpu;

    hwaddr entry;
    hwaddr initial_sp;
    hwaddr exit_trampoline;
    
    LinxUARTState uart;
} LinxVirtMachineState;

static hwaddr linx_align_up(hwaddr v, hwaddr align)
{
    if (align <= 1) {
        return v;
    }
    return (v + align - 1) & ~(align - 1);
}

/* Check if an address points to a BSTART instruction (C.BSTART or BSTART) */
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

/* Patch an ADDTPC instruction with a PC-relative offset.
 * ADDTPC: opcode = 0x07 (bits [6:0]), imm20 in bits [31:12], RegDst in bits [11:7]
 * Semantics: rd = PC + sext(imm20)
 */
static bool linx_patch_addtpc_pcrel(uint8_t *ram, size_t ram_size,
                                    hwaddr patch_addr, hwaddr target_addr,
                                    int64_t addend, Error **errp)
{
    uint32_t insn;
    int64_t delta;
    int32_t simm20;
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

    /* Calculate PC-relative offset (byte offset, not halfword scaled) */
    delta = (int64_t)(target_addr + addend) - (int64_t)patch_addr;
    
    simm20 = (int32_t)delta;
    if (simm20 < -(1 << 19) || simm20 >= (1 << 19)) {
        error_setg(errp,
                   "ADDTPC target out of range: patch @ 0x%" HWADDR_PRIx " -> 0x%" HWADDR_PRIx " (delta=%" PRId64 ")",
                   patch_addr, target_addr, delta);
        return false;
    }

    /* Encode imm20 in bits [31:12] */
    imm_bits = (uint32_t)(simm20 & 0xfffff);
    insn = (insn & 0xfff) | (imm_bits << 12);
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
 * These have opcode 0x69 in bits [6:0], with size in bits [14:12]
 * The immediate encoding is split: bits [11:7] and bits [31:15]
 */
static bool linx_patch_st_pcr(uint8_t *ram, size_t ram_size,
                              hwaddr patch_addr, hwaddr target_addr,
                              int64_t addend, Error **errp)
{
    uint32_t insn;
    int64_t delta;
    int64_t simm;
    uint32_t imm_lo, imm_hi;

    if (patch_addr + 4 > ram_size) {
        error_setg(errp, "ST.PCR relocation out of bounds @ 0x%" HWADDR_PRIx,
                   patch_addr);
        return false;
    }

    insn = ldl_le_p(ram + patch_addr);
    
    /* PC-relative byte offset */
    delta = (int64_t)(target_addr + addend) - (int64_t)patch_addr;

    /* Check range - need to fit in ~22 bits split encoding */
    simm = delta;
    if (simm < -(1 << 21) || simm >= (1 << 21)) {
        error_setg(errp,
                   "ST.PCR target out of range @ 0x%" HWADDR_PRIx " (delta=%" PRId64 ")",
                   patch_addr, delta);
        return false;
    }

    /* Split encoding: imm[4:0] -> bits [11:7], imm[21:5] -> bits [31:15] */
    imm_lo = (uint32_t)(simm & 0x1f);           /* bits [4:0] */
    imm_hi = (uint32_t)((simm >> 5) & 0x1ffff); /* bits [21:5] */
    
    insn = (insn & 0x7f) | (imm_lo << 7) | (imm_hi << 15);
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
    if (opcode == 0x07) {
        /* ADDTPC instruction - PC-relative data address */
        return linx_patch_addtpc_pcrel(ram, ram_size, patch_addr, target_addr,
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

    for (i = 0; i < eh->e_shnum; i++) {
        const Elf32_Shdr *sh = &shdrs[i];
        hwaddr align;

        if (!(sh->sh_flags & SHF_ALLOC) || sh->sh_size == 0) {
            continue;
        }
        align = sh->sh_addralign ? sh->sh_addralign : 1;
        if ((align & (align - 1)) != 0) {
            error_setg(errp, "invalid section alignment for #%zu", i);
            goto fail;
        }

        cur = linx_align_up(cur, align);
        if ((size_t)cur + sh->sh_size > ram_size) {
            error_setg(errp, "section #%zu does not fit in RAM", i);
            goto fail;
        }

        sec_addr[i] = cur;
        if (sh->sh_type == SHT_NOBITS) {
            memset(ram + cur, 0, sh->sh_size);
        } else {
            if ((size_t)sh->sh_offset + sh->sh_size > len) {
                error_setg(errp, "section #%zu data out of bounds", i);
                goto fail;
            }
            memcpy(ram + cur, buf + sh->sh_offset, sh->sh_size);
        }

        cur += sh->sh_size;
        if (cur > end) {
            end = cur;
        }
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
            break;
        }
    }
    if (*entry == 0) {
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
            hwaddr target;
            hwaddr patch_addr;
            uint32_t patch_insn;
            bool target_is_code;

            if (symidx >= nsyms || sym_addr[symidx] == 0) {
                error_setg(errp, "invalid relocation symbol index %u", symidx);
                goto fail;
            }

            target = sym_addr[symidx];
            patch_addr = base + rela[j].r_offset;
            
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
                if (symidx < nsyms) {
                    const Elf32_Sym *target_sym = &syms[symidx];
                    if (target_sym->st_shndx < eh->e_shnum) {
                        section_start = sec_addr[target_sym->st_shndx];
                    }
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

    for (i = 0; i < eh->e_shnum; i++) {
        const Elf64_Shdr *sh = &shdrs[i];
        hwaddr align;

        if (!(sh->sh_flags & SHF_ALLOC) || sh->sh_size == 0) {
            continue;
        }
        align = sh->sh_addralign ? sh->sh_addralign : 1;
        if ((align & (align - 1)) != 0) {
            error_setg(errp, "invalid section alignment for #%zu", i);
            goto fail;
        }

        cur = linx_align_up(cur, align);
        if ((size_t)cur + sh->sh_size > ram_size) {
            error_setg(errp, "section #%zu does not fit in RAM", i);
            goto fail;
        }

        sec_addr[i] = cur;
        if (sh->sh_type == SHT_NOBITS) {
            memset(ram + cur, 0, sh->sh_size);
        } else {
            if ((size_t)sh->sh_offset + sh->sh_size > len) {
                error_setg(errp, "section #%zu data out of bounds", i);
                goto fail;
            }
            memcpy(ram + cur, buf + sh->sh_offset, sh->sh_size);
        }

        cur += sh->sh_size;
        if (cur > end) {
            end = cur;
        }
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
            break;
        }
    }
    if (*entry == 0) {
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
            hwaddr target;
            hwaddr patch_addr;
            uint32_t patch_insn;
            bool target_is_code;

            if (symidx >= nsyms || sym_addr[symidx] == 0) {
                error_setg(errp, "invalid relocation symbol index %u", symidx);
                goto fail;
            }

            target = sym_addr[symidx];
            patch_addr = base + rela[j].r_offset;
            
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
                if (symidx < nsyms) {
                    const Elf64_Sym *target_sym = &syms[symidx];
                    if (target_sym->st_shndx < eh->e_shnum) {
                        section_start = sec_addr[target_sym->st_shndx];
                    }
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

static bool linx_load_elf_rel(const char *filename,
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
    ok = linx_load_elf32_rel(buf, len, ram, ram_size, load_base,
                             entry, image_end, errp);
#else
    ok = linx_load_elf64_rel(buf, len, ram, ram_size, load_base,
                             entry, image_end, errp);
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
}

static void linx_virt_init(MachineState *machine)
{
    LinxVirtMachineState *s = LINX_VIRT_MACHINE(machine);
    uint8_t *ram;
    hwaddr entry = 0;
    hwaddr image_end = 0;

    hwaddr load_base = 0x10000;
    hwaddr tramp;
    hwaddr sp;

    if (!machine->kernel_filename) {
        error_report("linx virt: missing -kernel <linxisa .o>");
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
    linx_uart_init(&s->uart);

    ram = memory_region_get_ram_ptr(machine->ram);
    if (!linx_load_elf_rel(machine->kernel_filename, ram, machine->ram_size,
                           load_base, &entry, &image_end, &error_fatal)) {
        exit(1);
    }

    if (linx_virt_debug_enabled()) {
        fprintf(stderr, "linx virt: loaded entry=0x%" HWADDR_PRIx " image_end=0x%" HWADDR_PRIx "\n",
                entry, image_end);
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
