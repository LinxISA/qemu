/*
 * Copyright (C) Huawei Technologies Co., Ltd. 2022. All rights reserved.
 * Description: Linx interrupt controller model
 * Author: Huawei OS Kernel Lab
 * Create: Tue Jul 05 10:20:53 2022
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "target/linx/cpu.h"
#include "hw/qdev-properties.h"
#include "hw/intc/lxintc.h"
#include "hw/sysbus.h"
#include "hw/pci/msi.h"
#include "hw/irq.h"
#include "exec/address-spaces.h"

/* This hash table mapped CPURISCVState to Controller */
GHashTable *env2ctl = NULL;

/* lxic domain array */
uint32_t lxic_domain[VIRT_LXIC_DOMAIN_WORD];
DeviceState *linx_intc[VIRT_CPUS_MAX][VIRT_LXIC_DOMAIN_NUM];

static uint32_t atomic_set_masked(uint32_t *a, uint32_t mask, uint32_t value)
{
    uint32_t old, new, cmp = qatomic_read(a);

    do {
        old = cmp;
        new = (old & ~mask) | (value & mask);
        cmp = qatomic_cmpxchg(a, old, new);
    } while (old != cmp);

    return old;
}

static void lxic_set_pending(LxicCtl *ctl, int dom, int irq, bool level)
{
    if (dom >= 0 && dom < VIRT_LXIC_DOMAIN_NUM && irq >= 0 && irq < LXIC_NIRQ)
        atomic_set_masked(&ctl->page[dom].pending[irq >> 5], 1 << (irq & 31), -!!level);
}

static void lxic_set_enable(LxicCtl *ctl, int dom, int irq, bool enable)
{
    if (dom >= 0 && dom < VIRT_LXIC_DOMAIN_NUM && irq >= 0 && irq < LXIC_NIRQ)
        atomic_set_masked(&ctl->page[dom].enable[irq >> 5], 1 << (irq & 31), -!!enable);
}

static void lxic_set_state(LxicCtl *ctl, int irq, bool level)
{
    if (irq >= 0 && irq < LXIC_NIRQ)
        atomic_set_masked(&ctl->state[irq >> 5], 1 << (irq & 31), -!!level);
}

static uint32_t lxic_get_state(LxicCtl *ctl, int irq)
{
    if (irq >= 0 && irq < LXIC_NIRQ && (ctl->state[irq >> 5] & (1 << (irq & 31))))
        return 1;
    return 0;
}

static void lxic_set_type(LxicCtl *ctl, int dom, int irq, uint32_t type)
{
    uint32_t i, j, mask;

    if (dom < 0 || dom >= VIRT_LXIC_DOMAIN_NUM || irq < 0 || irq >= LXIC_NIRQ)
        return;

    i = irq / 8;
    j = irq % 8;
    mask = IRQ_TYPE_SENSE_MASK << (j * 4);
    type = (type & IRQ_TYPE_SENSE_MASK) << (j * 4);

    if (irq >= 0 && irq < LXIC_NIRQ)
        atomic_set_masked(&ctl->page[dom].type[i], mask, type);
}

static uint32_t lxic_get_type(LxicCtl *ctl, int dom, int irq)
{
    uint32_t i, j;

    if (dom < 0 || dom >= VIRT_LXIC_DOMAIN_NUM || irq < 0 || irq >= LXIC_NIRQ)
        return 0;

    i = irq / 8;
    j = irq % 8;

    if (irq >= 0 && irq < LXIC_NIRQ)
        return (ctl->page[dom].type[i] & (IRQ_TYPE_SENSE_MASK << (j * 4))) >> (j * 4);
    return 0;
}

static void lxic_set_domain(int irq, uint32_t domain)
{
    uint32_t i, j, mask;

    if (domain >= VIRT_LXIC_DOMAIN_NUM)
        return;

    i = irq / 8;
    j = irq % 8;
    mask = IRQ_DOMAIN_MASK << (j * 4);
    domain = (domain & IRQ_DOMAIN_MASK) << (j * 4);

    if (irq >= 0 && irq < LXIC_NIRQ)
        atomic_set_masked(&lxic_domain[i], mask, domain);
}

static uint32_t lxic_get_domain(int irq)
{
    uint32_t i, j;

    i = irq / 8;
    j = irq % 8;

    if (irq >= 0 && irq < LXIC_NIRQ)
        return (lxic_domain[i] & (IRQ_DOMAIN_MASK << (j * 4))) >> (j * 4);

    return 0;
}

static int lxic_irqs_pending(LxicCtl *ctl, int dom, uint32_t irq)
{
    int i, j;

    i = irq >> 5;
    j = irq & 31;

    if (dom < 0 || dom >= VIRT_LXIC_DOMAIN_NUM || irq >= LXIC_NIRQ)
        return 0;

    /*
     * following three conditions should be met while uploading an interrupt
     * 1. pending
     * 2. enable
     * 3. priority <= threshold
     */

    return ((ctl->page[dom].pending[i] & ctl->page[dom].enable[i] & (1 << j)) && (irq <= ctl->page[dom].threshold)) ? 1 : 0;
}

static void lxic_update(LxicCtl *ctl)
{
    int dom, irq, level;

    if (ctl->lock_topei)
        return;

    /* choose highest priority interrupt and report it */
    for (dom = 0; dom < VIRT_LXIC_DOMAIN_NUM; dom++) {
        for (irq = 0; irq < LXIC_NIRQ; irq++) {
            level = lxic_irqs_pending(ctl, dom, irq);
            if (level) {
                ctl->topei = irq;
                qemu_set_irq(ctl->s_external_irq[dom][IRQ_LXIC_EXT], level);
                return;
            }
        }
    }
}

/* Access interrupt acknowledge register */
LINXException read_topei(CPULINXState *env, int ssrno, target_ulong *val)
{
    DeviceState **lxic = NULL;
    LxicCtl *ctl = NULL;
    int acr = get_field(ssrno, SYSREG_ADDR_ACR);

    lxic = g_hash_table_lookup(env2ctl, env);
    if (lxic == NULL) {
        *val = 0;
	return LINX_EXCP_NONE;
    }

    ctl = LXIC_CTL(lxic[acr]);
    if (ctl == NULL) {
        *val = 0;
        return LINX_EXCP_NONE;
    }

    if (ctl->topei != LXIC_NIRQ)
        ctl->lock_topei = 1;
    env->sysreg[acr].topei = ctl->topei;
    *val = env->sysreg[acr].topei;

    /* Clear pending bit, if irq is level triggered, then the pending bit
     * will be set again at eoi phase.
     * */
    lxic_set_pending(ctl, acr, *val, false);
    qemu_set_irq(ctl->s_external_irq[acr][IRQ_LXIC_EXT], 0);

    return LINX_EXCP_NONE;
}

LINXException read_eoiei(CPULINXState *env, int ssrno, target_ulong *val)
{
    int acr = get_field(ssrno, SYSREG_ADDR_ACR);

    *val = env->sysreg[acr].eoiei;
    return LINX_EXCP_NONE;
}

LINXException write_eoiei(CPULINXState *env, int ssrno, target_ulong val)
{
    DeviceState **lxic = NULL;
    LxicCtl *ctl = NULL;
    uint32_t type;
    int level;
    int acr = get_field(ssrno, SYSREG_ADDR_ACR);

    lxic = g_hash_table_lookup(env2ctl, env);
    if (lxic == NULL)
	return LINX_EXCP_NONE;

    ctl = LXIC_CTL(lxic[acr]);
    if (ctl == NULL)
        return LINX_EXCP_NONE;

    env->sysreg[acr].eoiei = val;
    ctl->topei = LXIC_NIRQ;
    ctl->lock_topei = 0;

    type = lxic_get_type(ctl, acr, val);
    level = lxic_get_state(ctl, val);

    if ( (type == IRQ_TYPE_LEVEL_HIGH && level > 0) ||
        (type == IRQ_TYPE_LEVEL_LOW && level <= 0) ) {
        lxic_set_pending(ctl, acr, val, true);
    }

    lxic_update(ctl);

    return LINX_EXCP_NONE;
}

static uint64_t lxic_mmio_read(void *opaque, hwaddr offset, unsigned int size)
{
    uint32_t word;
    uint64_t val;
    int dom;
    LxicCtl *ctl = LXIC_CTL(opaque);
    assert(ctl);

    dom = offset / VIRT_LXIC_DOMAIN_STRIDE;
    offset = offset % VIRT_LXIC_DOMAIN_STRIDE;

    if (dom < 0 || dom >= VIRT_LXIC_DOMAIN_NUM) {
        qemu_log("Error offset: %lx, error domain: %x\n", offset, dom);
        return 0;
    }

    // in order to support XLEN=32 and 64 both.
    if ((offset & 0x3) != 0)
        goto err;

    if (offset < CTL_PENDING_END) {
        word = (offset - CTL_PENDING_BASE) >> 2;
        return ctl->page[dom].pending[word];
    } else if (offset >= CTL_ENABLE_BASE && offset < CTL_ENABLE_END) {
        word = (offset - CTL_ENABLE_BASE) >> 2;
        return ctl->page[dom].enable[word];
    } else if (offset >= CTL_TYPE_BASE && offset < CTL_TYPE_END) {
        word = (offset - CTL_TYPE_BASE) >> 2;
	return ctl->page[dom].type[word];
    } else if (offset == CTL_SETVEC_BASE) {
        return ctl->page[dom].setvec;
    } else if (offset == CTL_CLRVEC_BASE) {
        return ctl->page[dom].clrvec;
    } else if (offset == CTL_ENVEC_BASE) {
        return ctl->page[dom].envec;
    } else if (offset == CTL_DISVEC_BASE) {
        return ctl->page[dom].disvec;
    } else if (offset == CTL_THRESHOLD_BASE) {
        return ctl->page[dom].threshold;
    } else if (offset == CTL_IPI_BASE) {
        val = ctl->page[dom].ipi;
        ctl->page[dom].ipi = 0;
        return val;
    } else if (offset == CTL_SETTYPE_BASE) {
        return ctl->page[dom].settype;
    } else if (offset == CTL_SETDOM_BASE) {
        return ctl->page[dom].setdomain;
    } else if (offset == CTL_GETDOM_BASE) {
        return ctl->page[dom].getdomain;
    } else {
        g_assert_not_reached();
    }

err:
    qemu_log_mask(LOG_GUEST_ERROR,
                  "%s: Invalid register read 0x%" HWADDR_PRIx "\n",
                  __func__, offset);
    return 0;
}

static void lxic_mmio_write(void *opaque, hwaddr offset,
                                uint64_t val, unsigned int size)
{
    int irq, dom, set, od, nd;
    uint32_t type;
    LxicCtl *ctl = LXIC_CTL(opaque);
    assert(ctl);

    // in order to support XLEN=32 and 64 both.
    if ((offset & 0x3) != 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: Invalid register write 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return;
    }

    dom = offset / VIRT_LXIC_DOMAIN_STRIDE;
    offset = offset % VIRT_LXIC_DOMAIN_STRIDE;

    if (dom < 0 || dom >= VIRT_LXIC_DOMAIN_NUM) {
        qemu_log("Error offset: %lx, error domain: %x\n", offset, dom);
	return;
    }

    if (offset < CTL_SETVEC_BASE) {
        qemu_log_mask(LOG_GUEST_ERROR, "failed to write pending or enable registers\n");
        return;
    } else if (offset == CTL_SETVEC_BASE) {
        ctl->page[dom].setvec = val;
        lxic_set_pending(ctl, dom, val, true);
        lxic_update(ctl);
    } else if (offset == CTL_CLRVEC_BASE) {
        ctl->page[dom].clrvec = val;
        lxic_set_pending(ctl, dom, val, false);
        lxic_update(ctl);
    } else if (offset == CTL_ENVEC_BASE) {
        ctl->page[dom].envec = val;
        lxic_set_enable(ctl, dom, val, true);
        lxic_update(ctl);
    } else if (offset == CTL_DISVEC_BASE) {
        ctl->page[dom].disvec = val;
        lxic_set_enable(ctl, dom, val, false);
        lxic_update(ctl);
    } else if (offset == CTL_THRESHOLD_BASE) {
        ctl->page[dom].threshold = val;
        lxic_update(ctl);
    } else if (offset == CTL_IPI_BASE) {
        ctl->page[dom].ipi = val;
        qemu_set_irq(ctl->s_external_irq[dom][IRQ_LXIC_SOFT], 1);
    } else if (offset == CTL_SETTYPE_BASE) {
        irq = val & (FDT_LXIC_NIRQ - 1);
        type = (val & (IRQ_TYPE_SENSE_MASK << 8)) >> 8;
        lxic_set_type(ctl, dom, irq, type);
    } else if (offset == CTL_SETDOM_BASE) {
        ctl->page[dom].setdomain = val;
        set = val & 0x1000;
        nd = (val & 0xf00) >> 8;
        irq = (val & 0xff);
        if (set) {
            od = lxic_get_domain(irq);
            if (nd < od) {
                lxic_set_domain(irq, nd);
                ctl->page[dom].getdomain = nd;
            }
        } else {
            od = lxic_get_domain(irq);
	    ctl->page[dom].getdomain = od;
        }
    } else if (offset == CTL_GETDOM_BASE) {
        qemu_log("Error: getdom register cannot be written\n");
    } else {
        g_assert_not_reached();
    }
}

static const MemoryRegionOps lxic_mmio_ops = {
        .read  = lxic_mmio_read,
        .write = lxic_mmio_write,
};

static void lxic_irq_request(void *opaque, int irq, int level)
{
    uint32_t dom;
    uint32_t type;
    int pre_level;
    LxicCtl *ctl = LXIC_CTL(opaque);

    dom = lxic_get_domain(irq);
    if (dom >= VIRT_LXIC_DOMAIN_NUM) {
        qemu_log("Error domain: %d\n", dom);
        return;
    }

    type = lxic_get_type(ctl, dom, irq);
    pre_level = lxic_get_state(ctl, irq);
    if (pre_level == level)
        return;
    lxic_set_state(ctl, irq, level > 0);

    /* edge interrupt */
    if (type & IRQ_TYPE_EDGE_BOTH) {
        if ((type == IRQ_TYPE_EDGE_RISING && level > 0) ||
            (type == IRQ_TYPE_EDGE_FALLING && level <= 0) ||
            (type == IRQ_TYPE_EDGE_BOTH)) {
            /* only set pending when level changes */
            lxic_set_pending(ctl, dom, irq, true);
            lxic_update(ctl);
        }
    /* level interrupt */
    } else {
        if (type == IRQ_TYPE_LEVEL_HIGH) {
            lxic_set_pending(ctl, dom, irq, level > 0);
            lxic_update(ctl);
        }

        if (type == IRQ_TYPE_LEVEL_LOW) {
            lxic_set_pending(ctl, dom, irq, level <= 0);
            lxic_update(ctl);
        }
    }
}

static void lxic_realize(DeviceState *dev, Error **errp)
{
    int i, j;
    LxicCtl *ctl = LXIC_CTL(dev);

    assert(ctl->mmio_base != 0);
    assert(ctl->mmio_size != 0);

    memory_region_init_io(&ctl->mmio, OBJECT(ctl), &lxic_mmio_ops,
                          ctl, "lxic.ctl.mmio", ctl->mmio_size);
    sysbus_init_mmio(SYS_BUS_DEVICE(ctl), &ctl->mmio);

    ctl->topei = LXIC_NIRQ;
    ctl->lock_topei = 0;

    for (i = 0; i < CTL_PENDING_WORD; i++)
        ctl->state[i] = 0;
    qdev_init_gpio_in(dev, lxic_irq_request, LXIC_NIRQ);

    for (i = 0; i < VIRT_LXIC_DOMAIN_NUM; i++) {
        qdev_init_gpio_out(dev, ctl->s_external_irq[i], NR_IRQ_PER_DOMAIN);
        ctl->page[i].threshold = 0;
        for (j = 0; j < CTL_PENDING_WORD; j++)
            ctl->page[i].pending[j] = 0;
        for (j = 0; j < CTL_ENABLE_WORD; j++)
            ctl->page[i].enable[j] = 0xFFFFFFFF;
        for (j = 0; j < CTL_TYPE_WORD; j++)
            ctl->page[i].type[j] = 0x11111111;
    }

    msi_nonbroken = true;
}

static Property lxic_properties[] = {
    DEFINE_PROP_UINT32("hart_id", LxicCtl, hart_id, 0),
    DEFINE_PROP_UINT32("mmio_base", LxicCtl, mmio_base, 0),
    DEFINE_PROP_UINT32("mmio_size", LxicCtl, mmio_size, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void lxic_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "Linx Interrupt Controller";
    dc->realize = lxic_realize;
    device_class_set_props(dc, lxic_properties);
}

static const TypeInfo lxic_info = {
    .name          = TYPE_LXIC_CTL,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(LxicCtl),
    .class_init    = lxic_class_init,
};

static void lxic_register_types(void)
{
    type_register_static(&lxic_info);
}

type_init(lxic_register_types)

DeviceState *lxic_create(uint32_t hart_id, uint32_t mmio_base,
                         uint32_t mmio_size)
{
    int i;
    DeviceState *ctl = qdev_new(TYPE_LXIC_CTL);
    CPUState *cpu = qemu_get_cpu(hart_id);

    qdev_prop_set_uint32(ctl, "hart_id", hart_id);
    qdev_prop_set_uint32(ctl, "mmio_base", mmio_base);
    qdev_prop_set_uint32(ctl, "mmio_size", mmio_size);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(ctl), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(ctl), 0, mmio_base);

    for (i = 0; i < VIRT_LXIC_DOMAIN_NUM; i++) {
        qdev_connect_gpio_out(ctl, IRQ_LXIC_EXT + i * NR_IRQ_PER_DOMAIN,
            qdev_get_gpio_in(DEVICE(cpu), ACR0_EI + i * PER_ACR_IRQ_NUM));

        qdev_connect_gpio_out(ctl, IRQ_LXIC_SOFT + i * NR_IRQ_PER_DOMAIN,
            qdev_get_gpio_in(DEVICE(cpu), ACR0_SI + i * PER_ACR_IRQ_NUM));
    }

    return ctl;
}

