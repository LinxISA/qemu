#ifndef HW_INTC_LXINTC_H
#define HW_INTC_LXINTC_H

#ifndef CONFIG_USER_ONLY

#include "target/linx/cpu.h"
#include "exec/cpu-defs.h"
#include "hw/sysbus.h"
#include "qom/object.h"

/* support maximum cpus: 8 */
#define VIRT_CPUS_MAX			8
/* 4 bits support 16 domains */
#define VIRT_LXIC_DOMAIN_BIT            4
/* domain array number */
#define VIRT_LXIC_DOMAIN_WORD           ((FDT_LXIC_NIRQ * VIRT_LXIC_DOMAIN_BIT) >> 5)
/* domain array size*/
#define VIRT_LXIC_DOMAIN_SIZE           (VIRT_LXIC_DOMAIN_WORD * 4)
/* support 2 domains */
#define VIRT_LXIC_DOMAIN_NUM            2
/* 2 domains: ACR0, ACR1 */
#define DOM_ACR0                        0
#define DOM_ACR1                        1

/*irq domain mask*/
#define IRQ_DOMAIN_MASK                 0xf

#define VIRT_LXIC_BASE                  0x4000000
#define VIRT_LXIC_DOMAIN_STRIDE         0x1000
#define VIRT_LXIC_STRIDE                (VIRT_LXIC_DOMAIN_STRIDE * VIRT_LXIC_DOMAIN_NUM)
#define VIRT_LXIC_SIZE(region)          (region * VIRT_LXIC_STRIDE)

#define FDT_LXINTC_INTERRUPT_CELLS      2
#define FDT_LXIC_NIRQ                   256
/* irq number is 0 - 254, 255 is invalid irq */
#define LXIC_NIRQ                       (FDT_LXIC_NIRQ - 1)

/* Support Soft and External IRQ */
#define NR_IRQ_PER_DOMAIN               2

#define IRQ_LXIC_EXT                    0
#define IRQ_LXIC_SOFT                   1

#define TYPE_LXIC_CTL "linx.ic"
typedef struct LxicCtl LxicCtl;
DECLARE_INSTANCE_CHECKER(LxicCtl, LXIC_CTL,
                         TYPE_LXIC_CTL)

#define CTL_PENDING_BIT     1
#define CTL_ENABLE_BIT      1
#define CTL_TYPE_BIT        4
#define CTL_PENDING_WORD    ((FDT_LXIC_NIRQ * CTL_PENDING_BIT) >> 5)
#define CTL_ENABLE_WORD     ((FDT_LXIC_NIRQ * CTL_ENABLE_BIT) >> 5)
#define CTL_TYPE_WORD       ((FDT_LXIC_NIRQ * CTL_TYPE_BIT) >> 5)

/* mmio registers */
#define CTL_PENDING_BASE    0
#define CTL_PENDING_END     (CTL_PENDING_BASE + CTL_PENDING_WORD * 4)
#define CTL_ENABLE_BASE     CTL_PENDING_END
#define CTL_ENABLE_END      (CTL_ENABLE_BASE + CTL_ENABLE_WORD * 4)
#define CTL_TYPE_BASE       CTL_ENABLE_END
#define CTL_TYPE_END        (CTL_TYPE_BASE + CTL_TYPE_WORD * 4)
#define CTL_SETVEC_BASE     CTL_TYPE_END
#define CTL_SETVEC_END      (CTL_SETVEC_BASE + 4)
#define CTL_CLRVEC_BASE     CTL_SETVEC_END
#define CTL_CLRVEC_END      (CTL_CLRVEC_BASE + 4)
#define CTL_ENVEC_BASE      CTL_CLRVEC_END
#define CTL_ENVEC_END       (CTL_ENVEC_BASE + 4)
#define CTL_DISVEC_BASE     CTL_ENVEC_END
#define CTL_DISVEC_END      (CTL_DISVEC_BASE + 4)
#define CTL_THRESHOLD_BASE  CTL_DISVEC_END
#define CTL_THRESHOLD_END   (CTL_THRESHOLD_BASE + 4)
#define CTL_IPI_BASE        CTL_THRESHOLD_END
#define CTL_IPI_END         (CTL_IPI_BASE + 4)
#define CTL_SETTYPE_BASE    CTL_IPI_END
#define CTL_SETTYPE_END     (CTL_SETTYPE_BASE + 4)
#define CTL_SETDOM_BASE     CTL_SETTYPE_END
#define CTL_SETDOM_END      (CTL_SETDOM_BASE + 4)
#define CTL_GETDOM_BASE     CTL_SETDOM_END
#define CTL_GETDOM_END      (CTL_GETDOM_BASE + 4)

/* IRQ type */
#define IRQ_TYPE_NONE                   0x0
#define IRQ_TYPE_EDGE_RISING            0x1
#define IRQ_TYPE_EDGE_FALLING           0x2
#define IRQ_TYPE_EDGE_BOTH              (IRQ_TYPE_EDGE_RISING | IRQ_TYPE_EDGE_FALLING)
#define IRQ_TYPE_LEVEL_HIGH             0x4
#define IRQ_TYPE_LEVEL_LOW              0x8
#define IRQ_TYPE_LEVEL_MASK             (IRQ_TYPE_LEVEL_LOW | IRQ_TYPE_LEVEL_HIGH)
#define IRQ_TYPE_SENSE_MASK             0xf

struct timer_info {
    qemu_irq *irq;
    QEMUTimer *timer;
    uint64_t intval;
};

struct LxicMmio {
    /*MMIO page*/
    uint32_t pending[CTL_PENDING_WORD];
    uint32_t enable[CTL_ENABLE_WORD];
    uint32_t type[CTL_TYPE_WORD];
    /* set interrupt vector */
    uint32_t setvec;
    /* clear interrupt vector */
    uint32_t clrvec;
    /* enable interrupt vector */
    uint32_t envec;
    /* disable interrupt vector */
    uint32_t disvec;
    /* interrupt threshold */
    uint32_t threshold;
    /* interrupt ipi */
    uint32_t ipi;
    /* set interrupt type: 4 bits type, 8 bit irq */
    uint32_t settype;
    /* set domain obtain parameters: 1 bit set/get, 4 bit domain, 8 bit irq */
    uint32_t setdomain;
    /* get interrupt domain */
    uint32_t getdomain;
};

struct LxicCtl {
    /*< private >*/
    SysBusDevice parent;

    /* MMIO params */
    MemoryRegion mmio;
    uint32_t mmio_base;
    uint32_t mmio_size;

    /* MMIO registers */
    struct LxicMmio page[VIRT_LXIC_DOMAIN_NUM];

    /* config */
    uint32_t hart_id;

    /* the level state of all pins */
    uint32_t state[CTL_PENDING_WORD];

    /* the register to record highest handling irq */
    uint32_t topei;
    uint32_t lock_topei;

    /* gpio out */
    qemu_irq s_external_irq[VIRT_LXIC_DOMAIN_NUM][NR_IRQ_PER_DOMAIN];
};

DeviceState *lxic_create(uint32_t hart_id, uint32_t mmio_base, uint32_t mmio_size);
LINXException read_topei(CPULINXState *env, int ssrno, target_ulong *val);
LINXException read_eoiei(CPULINXState *env, int ssrno, target_ulong *val);
LINXException write_eoiei(CPULINXState *env, int ssrno, target_ulong val);
extern GHashTable *env2ctl;
extern uint32_t lxic_domain[VIRT_LXIC_DOMAIN_WORD];
extern DeviceState *linx_intc[VIRT_CPUS_MAX][VIRT_LXIC_DOMAIN_NUM];

#endif

#endif
