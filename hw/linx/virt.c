/*
 * QEMU LinxISA "virt" machine
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/boards.h"
#include "system/address-spaces.h"
#include "system/reset.h"

#include "cpu.h"

#include <elf.h>

#define TYPE_LINX_VIRT_MACHINE MACHINE_TYPE_NAME("virt")
OBJECT_DECLARE_SIMPLE_TYPE(LinxVirtMachineState, LINX_VIRT_MACHINE)

typedef struct LinxVirtMachineState {
    MachineState parent_obj;

    MemoryRegion ram;
    LinxCPU *cpu;

    hwaddr entry;
    hwaddr initial_sp;
    hwaddr exit_trampoline;
} LinxVirtMachineState;

static hwaddr linx_align_up(hwaddr v, hwaddr align)
{
    if (align <= 1) {
        return v;
    }
    return (v + align - 1) & ~(align - 1);
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
            *entry = sym_addr[i];
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

            if (symidx >= nsyms || sym_addr[symidx] == 0) {
                error_setg(errp, "invalid relocation symbol index %u", symidx);
                goto fail;
            }

            target = sym_addr[symidx];
            patch_addr = base + rela[j].r_offset;

            if (!linx_patch_bstart_call_pcrel(ram, ram_size, patch_addr, target,
                                             (int64_t)rela[j].r_addend, errp)) {
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
            *entry = sym_addr[i];
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

            if (symidx >= nsyms || sym_addr[symidx] == 0) {
                error_setg(errp, "invalid relocation symbol index %u", symidx);
                goto fail;
            }

            target = sym_addr[symidx];
            patch_addr = base + rela[j].r_offset;

            if (!linx_patch_bstart_call_pcrel(ram, ram_size, patch_addr, target,
                                             rela[j].r_addend, errp)) {
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

    memory_region_init_ram(&s->ram, OBJECT(machine), "linx.virt.ram",
                           machine->ram_size, &error_fatal);
    memory_region_add_subregion(get_system_memory(), 0, &s->ram);

    s->cpu = LINX_CPU(cpu_create(machine->cpu_type));

    ram = memory_region_get_ram_ptr(&s->ram);
    if (!linx_load_elf_rel(machine->kernel_filename, ram, machine->ram_size,
                           load_base, &entry, &image_end, &error_fatal)) {
        exit(1);
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
