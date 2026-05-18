/* Support for writing ELF notes for LINX architectures
 *
 * Copyright (C) 2021 Huawei Technologies Co., Ltd
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "elf.h"
#include "sysemu/dump.h"

/*
 * FIXME: After defining the format of the Linx core file, we need to modify
 * the linx_user_regs and so on.
 */

/* struct user_regs_struct from arch/riscv/include/uapi/asm/ptrace.h */
struct linx_user_regs {
    uint64_t pc;
    uint64_t regs[24];
} QEMU_PACKED;

QEMU_BUILD_BUG_ON(sizeof(struct linx_user_regs) != 200);

/* struct elf_prstatus from include/linux/elfcore.h */
struct linx_elf_prstatus {
    char pad1[32]; /* 32 == offsetof(struct elf_prstatus, pr_pid) */
    uint32_t pr_pid;
    char pad2[76]; /* 76 == offsetof(struct elf_prstatus, pr_reg) -
                            offsetof(struct elf_prstatus, pr_ppid) */
    struct linx_user_regs pr_reg;
    char pad3[8];
} QEMU_PACKED;

QEMU_BUILD_BUG_ON(sizeof(struct linx_elf_prstatus) != 320);

struct linx_note {
    Elf64_Nhdr hdr;
    char name[8]; /* align_up(sizeof("CORE"), 4) */
    struct linx_elf_prstatus prstatus;
} QEMU_PACKED;

#define LINX_NOTE_HEADER_SIZE offsetof(struct linx_note, prstatus)
#define LINX_PRSTATUS_NOTE_SIZE \
            (LINX_NOTE_HEADER_SIZE + sizeof(struct linx_elf_prstatus))

static void linx_note_init(struct linx_note *note, DumpState *s,
                            const char *name, Elf64_Word namesz,
                            Elf64_Word type, Elf64_Word descsz)
{
    memset(note, 0, sizeof(*note));

    note->hdr.n_namesz = cpu_to_dump32(s, namesz);
    note->hdr.n_descsz = cpu_to_dump32(s, descsz);
    note->hdr.n_type = cpu_to_dump32(s, type);

    memcpy(note->name, name, namesz);
}

int linx_cpu_write_elf_note(WriteCoreDumpFunction f, CPUState *cs,
                             int cpuid, void *opaque)
{
    struct linx_note note;
    LINXCPU *cpu = LINX_CPU(cs);
    CPULINXState *env = &cpu->env;
    DumpState *s = opaque;
    int ret, i = 0;
    const char name[] = "CORE";

    linx_note_init(&note, s, name, sizeof(name),
                    NT_PRSTATUS, sizeof(note.prstatus));

    note.prstatus.pr_pid = cpu_to_dump32(s, cpuid);

    note.prstatus.pr_reg.pc = cpu_to_dump64(s, env->pc);

    for (i = 0; i < 16; i++) {
        /* linx need grp[0] and no exist zero register */
        note.prstatus.pr_reg.regs[i] = cpu_to_dump64(s, env->gpr[i]);
    }

    ret = f(&note, LINX_PRSTATUS_NOTE_SIZE, s);
    if (ret < 0) {
        return -1;
    }

    return ret;
}

int cpu_get_dump_info(ArchDumpInfo *info,
                      const GuestPhysBlockList *guest_phys_blocks)
{
    info->d_machine = EM_LINX;

    info->d_class = ELFCLASS64;

    info->d_endian = ELFDATA2LSB;

    return 0;
}

ssize_t cpu_get_note_size(int class, int machine, int nr_cpus)
{
    size_t note_size;

    note_size = LINX_PRSTATUS_NOTE_SIZE;

    return note_size * nr_cpus;
}
