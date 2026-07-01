/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ASM_LINX_PERCPU_H
#define _ASM_LINX_PERCPU_H

/*
 * LinxISA bring-up percpu helpers.
 *
 * The generic percpu implementation uses `u128` for this_cpu_try_cmpxchg128().
 * Early LinxISA toolchain/backend support for 128-bit loads/stores/compares is
 * still maturing. SLUB relies on this_cpu_try_cmpxchg128() for the per-cpu
 * (freelist, tid) pair and will livelock if the operation never succeeds.
 *
 * Provide a simple irq-disabled, 2x64-bit implementation for !SMP bring-up.
 */

#include <linux/irqflags.h>
#include <linux/string.h>
#include <linux/types.h>

#include <asm-generic/percpu.h>

#ifdef CONFIG_64BIT

static __always_inline bool linx___pcpu_try_cmpxchg128(u128 __percpu *pcp,
						       u128 *ovalp, u128 nval)
{
	unsigned long flags;
	u64 cur[2];
	u64 old[2];
	u64 newv[2];
	u64 *p;
	bool ret;

	memcpy(old, ovalp, sizeof(old));
	memcpy(newv, &nval, sizeof(newv));

	raw_local_irq_save(flags);
	p = (u64 *)raw_cpu_ptr(pcp);
	cur[0] = READ_ONCE(p[0]);
	cur[1] = READ_ONCE(p[1]);

	if (cur[0] == old[0] && cur[1] == old[1]) {
		WRITE_ONCE(p[0], newv[0]);
		WRITE_ONCE(p[1], newv[1]);
		ret = true;
	} else {
		memcpy(ovalp, cur, sizeof(cur));
		ret = false;
	}
	raw_local_irq_restore(flags);

	return ret;
}

static __always_inline u128 linx___pcpu_cmpxchg128(u128 __percpu *pcp,
						   u128 oval, u128 nval)
{
	unsigned long flags;
	u64 cur[2];
	u64 old[2];
	u64 newv[2];
	u64 *p;
	u128 ret;

	memcpy(old, &oval, sizeof(old));
	memcpy(newv, &nval, sizeof(newv));

	raw_local_irq_save(flags);
	p = (u64 *)raw_cpu_ptr(pcp);
	cur[0] = READ_ONCE(p[0]);
	cur[1] = READ_ONCE(p[1]);

	if (cur[0] == old[0] && cur[1] == old[1]) {
		WRITE_ONCE(p[0], newv[0]);
		WRITE_ONCE(p[1], newv[1]);
		memcpy(&ret, old, sizeof(old));
	} else {
		memcpy(&ret, cur, sizeof(cur));
	}
	raw_local_irq_restore(flags);

	return ret;
}

#undef raw_cpu_try_cmpxchg128
#define raw_cpu_try_cmpxchg128(pcp, ovalp, nval) \
	linx___pcpu_try_cmpxchg128(&(pcp), (ovalp), (nval))

#undef raw_cpu_cmpxchg128
#define raw_cpu_cmpxchg128(pcp, oval, nval) \
	linx___pcpu_cmpxchg128(&(pcp), (oval), (nval))

#undef this_cpu_try_cmpxchg128
#define this_cpu_try_cmpxchg128(pcp, ovalp, nval) \
	linx___pcpu_try_cmpxchg128(&(pcp), (ovalp), (nval))

#undef this_cpu_cmpxchg128
#define this_cpu_cmpxchg128(pcp, oval, nval) \
	linx___pcpu_cmpxchg128(&(pcp), (oval), (nval))

#endif /* CONFIG_64BIT */

#endif /* _ASM_LINX_PERCPU_H */
