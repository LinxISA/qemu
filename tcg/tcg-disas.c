/** Derived from linux-user/elfload.c */

#include "qemu/osdep.h"
#include "cpu.h"

#if TARGET_LONG_BITS == 64
# define ELF_CLASS ELFCLASS64
#else
# define ELF_CLASS ELFCLASS32
#endif

#ifdef TARGET_WORDS_BIGENDIAN
# define ELF_DATA   ELFDATA2MSB
#else
# define ELF_DATA   ELFDATA2LSB
#endif

#include "sysemu/tcg.h"
#include "elf.h"
#include "disas/disas.h"
#include "qemu/bitops.h"
#include "qemu/path.h"
#include "qemu/queue.h"
#include "qemu/bswap.h"
#include "qemu/units.h"
#include "qapi/error.h"

#ifdef BSWAP_NEEDED
static void bswap_ehdr(struct elfhdr *ehdr)
{
    bswap16s(&ehdr->e_type);            /* Object file type */
    bswap16s(&ehdr->e_machine);         /* Architecture */
    bswap32s(&ehdr->e_version);         /* Object file version */
    bswaptls(&ehdr->e_entry);           /* Entry point virtual address */
    bswaptls(&ehdr->e_phoff);           /* Program header table file offset */
    bswaptls(&ehdr->e_shoff);           /* Section header table file offset */
    bswap32s(&ehdr->e_flags);           /* Processor-specific flags */
    bswap16s(&ehdr->e_ehsize);          /* ELF header size in bytes */
    bswap16s(&ehdr->e_phentsize);       /* Program header table entry size */
    bswap16s(&ehdr->e_phnum);           /* Program header table entry count */
    bswap16s(&ehdr->e_shentsize);       /* Section header table entry size */
    bswap16s(&ehdr->e_shnum);           /* Section header table entry count */
    bswap16s(&ehdr->e_shstrndx);        /* Section header string table index */
}

__attribute__((unused))
static void bswap_phdr(struct elf_phdr *phdr, int phnum)
{
    int i;
    for (i = 0; i < phnum; ++i, ++phdr) {
        bswap32s(&phdr->p_type);        /* Segment type */
        bswap32s(&phdr->p_flags);       /* Segment flags */
        bswaptls(&phdr->p_offset);      /* Segment file offset */
        bswaptls(&phdr->p_vaddr);       /* Segment virtual address */
        bswaptls(&phdr->p_paddr);       /* Segment physical address */
        bswaptls(&phdr->p_filesz);      /* Segment size in file */
        bswaptls(&phdr->p_memsz);       /* Segment size in memory */
        bswaptls(&phdr->p_align);       /* Segment alignment */
    }
}

static void bswap_shdr(struct elf_shdr *shdr, int shnum)
{
    int i;
    for (i = 0; i < shnum; ++i, ++shdr) {
        bswap32s(&shdr->sh_name);
        bswap32s(&shdr->sh_type);
        bswaptls(&shdr->sh_flags);
        bswaptls(&shdr->sh_addr);
        bswaptls(&shdr->sh_offset);
        bswaptls(&shdr->sh_size);
        bswap32s(&shdr->sh_link);
        bswap32s(&shdr->sh_info);
        bswaptls(&shdr->sh_addralign);
        bswaptls(&shdr->sh_entsize);
    }
}

static void bswap_sym(struct elf_sym *sym)
{
    bswap32s(&sym->st_name);
    bswaptls(&sym->st_value);
    bswaptls(&sym->st_size);
    bswap16s(&sym->st_shndx);
}

#ifdef TARGET_MIPS
static void bswap_mips_abiflags(Mips_elf_abiflags_v0 *abiflags)
{
    bswap16s(&abiflags->version);
    bswap32s(&abiflags->ases);
    bswap32s(&abiflags->isa_ext);
    bswap32s(&abiflags->flags1);
    bswap32s(&abiflags->flags2);
}
#endif
#else
static inline void bswap_ehdr(struct elfhdr *ehdr) { }
static inline void bswap_phdr(struct elf_phdr *phdr, int phnum) { }
static inline void bswap_shdr(struct elf_shdr *shdr, int shnum) { }
static inline void bswap_sym(struct elf_sym *sym) { }
#ifdef TARGET_MIPS
static inline void bswap_mips_abiflags(Mips_elf_abiflags_v0 *abiflags) { }
#endif
#endif

/* Verify the portions of EHDR within E_IDENT for the target.
   This can be performed before bswapping the entire header.  */
static bool elf_check_ident(struct elfhdr *ehdr)
{
    return (ehdr->e_ident[EI_MAG0] == ELFMAG0
            && ehdr->e_ident[EI_MAG1] == ELFMAG1
            && ehdr->e_ident[EI_MAG2] == ELFMAG2
            && ehdr->e_ident[EI_MAG3] == ELFMAG3
            && ehdr->e_ident[EI_CLASS] == ELF_CLASS
            && ehdr->e_ident[EI_DATA] == ELF_DATA
            && ehdr->e_ident[EI_VERSION] == EV_CURRENT);
}

/* Verify the portions of EHDR outside of E_IDENT for the target.
   This has to wait until after bswapping the header.  */
static bool elf_check_ehdr(struct elfhdr *ehdr)
{
    return (ehdr->e_ehsize == sizeof(struct elfhdr)
            && ehdr->e_phentsize == sizeof(struct elf_phdr)
            && (ehdr->e_type == ET_EXEC || ehdr->e_type == ET_DYN));
}

static int symfind(const void *s0, const void *s1)
{
    target_ulong addr = *(target_ulong *)s0;
    struct elf_sym *sym = (struct elf_sym *)s1;
    int result = 0;
    if (addr < sym->st_value) {
        result = -1;
    } else if (addr >= sym->st_value + sym->st_size) {
        result = 1;
    }
    return result;
}

static const char *lookup_symbolxx(struct syminfo *s, target_ulong orig_addr)
{
#if ELF_CLASS == ELFCLASS32
    struct elf_sym *syms = s->disas_symtab.elf32;
#else
    struct elf_sym *syms = s->disas_symtab.elf64;
#endif

    // binary search
    struct elf_sym *sym;

    sym = bsearch(&orig_addr, syms, s->disas_num_syms, sizeof(*syms), symfind);
    if (sym != NULL) {
        return s->disas_strtab + sym->st_name;
    }

    return "";
}

/* FIXME: This should use elf_ops.h  */
static int symcmp(const void *s0, const void *s1)
{
    struct elf_sym *sym0 = (struct elf_sym *)s0;
    struct elf_sym *sym1 = (struct elf_sym *)s1;
    return (sym0->st_value < sym1->st_value)
        ? -1
        : ((sym0->st_value > sym1->st_value) ? 1 : 0);
}

static void load_symbols(struct elfhdr *hdr, int fd)
{
    int i, shnum, nsyms, sym_idx = 0, str_idx = 0;
    uint64_t segsz;
    struct elf_shdr *shdr;
    char *strings = NULL;
    struct syminfo *s = NULL;
    struct elf_sym *new_syms, *syms = NULL;

    shnum = hdr->e_shnum;
    i = shnum * sizeof(struct elf_shdr);
    shdr = (struct elf_shdr *)alloca(i);
    if (pread(fd, shdr, i, hdr->e_shoff) != i) {
        return;
    }

    bswap_shdr(shdr, shnum);
    for (i = 0; i < shnum; ++i) {
        if (shdr[i].sh_type == SHT_SYMTAB) {
            sym_idx = i;
            str_idx = shdr[i].sh_link;
            goto found;
        }
    }

    error_report("symbol file contains no symbols\n");

 found:
    /* Now know where the strtab and symtab are.  Snarf them.  */
    s = g_try_new(struct syminfo, 1);
    if (!s) {
        goto give_up;
    }

    segsz = shdr[str_idx].sh_size;
    s->disas_strtab = strings = g_try_malloc(segsz);
    if (!strings ||
        pread(fd, strings, segsz, shdr[str_idx].sh_offset) != segsz) {
        goto give_up;
    }

    segsz = shdr[sym_idx].sh_size;
    syms = g_try_malloc(segsz);
    if (!syms || pread(fd, syms, segsz, shdr[sym_idx].sh_offset) != segsz) {
        goto give_up;
    }

    if (segsz / sizeof(struct elf_sym) > INT_MAX) {
        /* Implausibly large symbol table: give up rather than ploughing
         * on with the number of symbols calculation overflowing
         */
        goto give_up;
    }
    nsyms = segsz / sizeof(struct elf_sym);
    for (i = 0; i < nsyms; ) {
        bswap_sym(syms + i);
        /* Throw away entries which we do not need.  */
        if (syms[i].st_shndx == SHN_UNDEF
            || syms[i].st_shndx >= SHN_LORESERVE
            || ((ELF_ST_TYPE(syms[i].st_info) != STT_FUNC)
            && (ELF_ST_TYPE(syms[i].st_info) != STT_NOTYPE))) {
            if (i < --nsyms) {
                syms[i] = syms[nsyms];
            }
        } else {
#if defined(TARGET_ARM) || defined (TARGET_MIPS)
            /* The bottom address bit marks a Thumb or MIPS16 symbol.  */
            syms[i].st_value &= ~(target_ulong)1;
#endif
            i++;
        }
    }

    /* No "useful" symbol.  */
    if (nsyms == 0) {
        goto give_up;
    }

    /* Attempt to free the storage associated with the local symbols
       that we threw away.  Whether or not this has any effect on the
       memory allocation depends on the malloc implementation and how
       many symbols we managed to discard.  */
    new_syms = g_try_renew(struct elf_sym, syms, nsyms);
    if (new_syms == NULL) {
        goto give_up;
    }
    syms = new_syms;

    qsort(syms, nsyms, sizeof(*syms), symcmp);

    s->disas_num_syms = nsyms;
#if ELF_CLASS == ELFCLASS32
    s->disas_symtab.elf32 = syms;
#else
    s->disas_symtab.elf64 = syms;
#endif
    s->lookup_symbol = lookup_symbolxx;
    s->next = syminfos;
    syminfos = s;

    return;

give_up:
    g_free(s);
    g_free(strings);
    g_free(syms);
}

void disas_add_symfile(const char *filename) {
    int fd, retval;
    Error *err = NULL;
#define BPRM_BUF_SIZE 1024
    char bprm_buf[BPRM_BUF_SIZE];


    fd = open(path(filename), O_RDONLY);
    if (fd < 0) {
        error_setg_file_open(&err, errno, filename);
        error_report_err(err);
        goto end;
    }

    retval = read(fd, bprm_buf, BPRM_BUF_SIZE);
    if (retval < 0) {
        error_setg_errno(&err, errno, "Error reading file header");
        error_reportf_err(err, "%s: ", filename);
        goto end;
    }

    if (retval < BPRM_BUF_SIZE) {
        memset(bprm_buf + retval, 0, BPRM_BUF_SIZE - retval);
    }

    struct elfhdr *ehdr = (struct elfhdr *)bprm_buf;
    if (!elf_check_ident(ehdr)) {
        error_setg(&err, "Invalid ELF image for this architecture");
        goto end;
    }
    bswap_ehdr(ehdr);
    if (!elf_check_ehdr(ehdr)) {
        error_setg(&err, "Invalid ELF image for this architecture");
        goto end;
    }

    load_symbols(ehdr, fd);

    return;

end:
    exit(-1);
}
