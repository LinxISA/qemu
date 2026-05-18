#ifndef _CMPXCHG_H_
#define _CMPXCHG_H_

#define BUILD_BUG() __asm__ __volatile__ ("nop")

#define ASM_FENCE_BLOCK(label, pred, succ)		\
	"" #label ":				\n"	\
	"bstart " #label "0f			\n"	\
	"b.sys					\n"	\
	"bstop  " #label "1f			\n"	\
	PUSH_BLOCK_TEXT_BODY_SECTION			\
	"" #label "0:				\n"	\
		"fence " #pred ", " #succ "	\n"	\
	"" #label "1:				\n"	\
	POP_BLOCK_TEXT_BODY_SECTION

#define ASM_RELEASE_BARRIER_BLOCK(label) ASM_FENCE_BLOCK(label, rw, r)
#define ASM_ACQUIRE_BARRIER_BLOCK(label) ASM_FENCE_BLOCK(label, r, rw)
#define ASM_FULL_BARRIER_BLOCK(label) ASM_FENCE_BLOCK(label, rw, rw)

/*
 * define an stringify atomic swap block used in extended asm
 *
 * @label:
 *     block label, a non-zero decimal number,
 *     which must be different from the one used in
 *     @release_barrier and @acquire_barrier
 * @type:
 *     w for 4 bytes integer and d for 8 bytes integer
 * @release_barrier, @acquire_barrier:
 *     optional memory fence block, define with ASM_FENCE_BLOCK
 * @order:
 *     optional block atomic.order attribute string, e.g. ".aqrl"
 * @ret, @new, @ptr:
 *     ret = *ptr, *ptr = new, return @ret
 */
#define ASM_AMOSWAP(label, type,				\
		release_barrier, acquire_barrier, order,	\
		ret, new, ptr)					\
	__asm__ __volatile__ (					\
		"" release_barrier			"\n"	\
		"" #label ":				\n"	\
		"bstart " #label "0f			\n"	\
		"b.std					\n"	\
		"battr atomic" order "			\n"	\
		"bget %[p], %[n]			\n"	\
		"bset %[r]				\n"	\
		"bstop  " #label "1f			\n"	\
		PUSH_BLOCK_TEXT_BODY_SECTION			\
		"" #label "0:				\n"	\
			"get %[p]			\n"	\
			"l" #type " [t#1, 0]		\n"	\
			"get %[n]			\n"	\
			"s" #type " t#1, [t#3, 0]	\n"	\
			"set %[r], t#3			\n"	\
		"" #label "1:				\n"	\
		POP_BLOCK_TEXT_BODY_SECTION			\
		"" acquire_barrier			"\n"	\
		: [r] "=r" (ret)				\
		: [p] "r" (ptr), [n] "r" (new)			\
		: "memory")

#define __xchg_relaxed(ptr, new, size)					\
({									\
	__typeof__(ptr) __ptr = (ptr);					\
	__typeof__(new) __new = (new);					\
	__typeof__(*(ptr)) __ret;					\
	switch (size) {							\
	case 4:								\
		ASM_AMOSWAP(1, w, , , , __ret, __new, __ptr);		\
		break;							\
	case 8:								\
		ASM_AMOSWAP(1, d, , , , __ret, __new, __ptr);		\
		break;							\
	default:							\
		BUILD_BUG();						\
	}								\
	__ret;								\
})

#define arch_xchg_relaxed(ptr, x)					\
({									\
	__typeof__(*(ptr)) _x_ = (x);					\
	(__typeof__(*(ptr))) __xchg_relaxed((ptr),			\
					    _x_, sizeof(*(ptr)));	\
})

#define __xchg_acquire(ptr, new, size)					\
({									\
	__typeof__(ptr) __ptr = (ptr);					\
	__typeof__(new) __new = (new);					\
	__typeof__(*(ptr)) __ret;					\
	switch (size) {							\
	case 4:								\
		ASM_AMOSWAP(1, w, , ASM_ACQUIRE_BARRIER_BLOCK(2), ,	\
			     __ret, __new, __ptr);			\
		break;							\
	case 8:								\
		ASM_AMOSWAP(1, d, , ASM_ACQUIRE_BARRIER_BLOCK(2), ,	\
			     __ret, __new, __ptr);			\
		break;							\
	default:							\
		BUILD_BUG();						\
	}								\
	__ret;								\
})

#define arch_xchg_acquire(ptr, x)					\
({									\
	__typeof__(*(ptr)) _x_ = (x);					\
	(__typeof__(*(ptr))) __xchg_acquire((ptr),			\
					    _x_, sizeof(*(ptr)));	\
})

#define __xchg_release(ptr, new, size)					\
({									\
	__typeof__(ptr) __ptr = (ptr);					\
	__typeof__(new) __new = (new);					\
	__typeof__(*(ptr)) __ret;					\
	switch (size) {							\
	case 4:								\
		ASM_AMOSWAP(1, w, ASM_RELEASE_BARRIER_BLOCK(2), , ,	\
			     __ret, __new, __ptr);			\
		break;							\
	case 8:								\
		ASM_AMOSWAP(1, d, ASM_RELEASE_BARRIER_BLOCK(2), , ,	\
			     __ret, __new, __ptr);			\
		break;							\
	default:							\
		BUILD_BUG();						\
	}								\
	__ret;								\
})

#define arch_xchg_release(ptr, x)					\
({									\
	__typeof__(*(ptr)) _x_ = (x);					\
	(__typeof__(*(ptr))) __xchg_release((ptr),			\
					    _x_, sizeof(*(ptr)));	\
})

#define __xchg(ptr, new, size)						\
({									\
	__typeof__(ptr) __ptr = (ptr);					\
	__typeof__(new) __new = (new);					\
	__typeof__(*(ptr)) __ret;					\
	switch (size) {							\
	case 4:								\
		ASM_AMOSWAP(1, w, , , ".aqrl", __ret, __new, __ptr);	\
		break;							\
	case 8:								\
		ASM_AMOSWAP(1, d, , , ".aqrl", __ret, __new, __ptr);	\
		break;							\
	default:							\
		BUILD_BUG();						\
	}								\
	__ret;								\
})

#define arch_xchg(ptr, x)						\
({									\
	__typeof__(*(ptr)) _x_ = (x);					\
	(__typeof__(*(ptr))) __xchg((ptr), _x_, sizeof(*(ptr)));	\
})

#define xchg32(ptr, x)							\
({									\
	BUILD_BUG_ON(sizeof(*(ptr)) != 4);				\
	arch_xchg((ptr), (x));						\
})

#define xchg64(ptr, x)							\
({									\
	BUILD_BUG_ON(sizeof(*(ptr)) != 8);				\
	arch_xchg((ptr), (x));						\
})

/*
 * Atomic compare and exchange.  Compare OLD with MEM, if identical,
 * store NEW in MEM.  Return the initial value in MEM.  Success is
 * indicated by comparing RETURN with OLD.
 */

/*
 * define an stringify atomic compare and exchange block used in extended asm.
 *
 * @label_lr, @label_sc, @label_end:
 *     block label, a non-zero decimal number, which must be different
 *     from the one used in @release_barrier and @acquire_barrier
 * @type:
 *     w for 4 bytes integer and d for 8 bytes integer
 * @release_barrier, @acquire_barrier:
 *     optional memory fence block, define with ASM_FENCE_BLOCK
 * @order:
 *     optional sc block atomic.order attribute string, e.g. ".rl"
 * @ret, @new, @old, @ptr:
 *     ret = *ptr, if (old == ret) *ptr = new, return @ret
 */
#define ASM_CMPXCHG(label_lr, label_sc, label_end, type,	\
		release_barrier, acquire_barrier, order,	\
		ret, new, old, ptr)				\
	__asm__ __volatile__ (					\
		"" release_barrier			"\n"	\
		"" #label_lr ":				\n"	\
		"bstart " #label_lr "0f			\n"	\
		"b.sys					\n"	\
		"battr atomic				\n"	\
		"bget %[p], %[o]			\n"	\
		"bset %[r]				\n"	\
		"bnext.cond " #label_end "f		\n"	\
		"bstop  " #label_lr "1f			\n"	\
		PUSH_BLOCK_TEXT_BODY_SECTION			\
		"" #label_lr "0:			\n"	\
			"get %[p]			\n"	\
			"lr." #type " [t#1]		\n"	\
			"set %[r], t#1			\n"	\
			"get %[o]			\n"	\
			"setc.ne t#1, t#3		\n"	\
		"" #label_lr "1:			\n"	\
		POP_BLOCK_TEXT_BODY_SECTION			\
		"" #label_sc ":				\n"	\
		"bstart " #label_sc "0f			\n"	\
		"b.sys					\n"	\
		"battr atomic" order "			\n"	\
		"bget %[p], %[n]			\n"	\
		"bnext.cond " #label_lr "b		\n"	\
		"bstop  " #label_sc "1f			\n"	\
		PUSH_BLOCK_TEXT_BODY_SECTION			\
		"" #label_sc "0:			\n"	\
			"get %[p]			\n"	\
			"get %[n]			\n"	\
			"sc." #type " t#1, [t#2]	\n"	\
			"setc.nei t#1, 0		\n"	\
		"" #label_sc "1:			\n"	\
		POP_BLOCK_TEXT_BODY_SECTION			\
		"" acquire_barrier			"\n"	\
		"" #label_end ":			\n"	\
		: [r] "=&r" (ret)				\
		: [p] "r" (ptr), [o] "r" (old), [n] "r" (new)	\
		: "memory")

#define __cmpxchg_relaxed(ptr, old, new, size)				\
({									\
	__typeof__(ptr) __ptr = (ptr);					\
	__typeof__(*(ptr)) __old = (old);				\
	__typeof__(*(ptr)) __new = (new);				\
	__typeof__(*(ptr)) __ret;					\
	switch (size) {							\
	case 4:								\
		ASM_CMPXCHG(1, 2, 3, w, , , ,				\
			__ret, __new, (long)__old, __ptr);		\
		break;							\
	case 8:								\
		ASM_CMPXCHG(1, 2, 3, d, , , ,				\
			__ret, __new, __old, __ptr);			\
		break;							\
	default:							\
		BUILD_BUG();						\
	}								\
	__ret;								\
})

#define arch_cmpxchg_relaxed(ptr, o, n)					\
({									\
	__typeof__(*(ptr)) _o_ = (o);					\
	__typeof__(*(ptr)) _n_ = (n);					\
	(__typeof__(*(ptr))) __cmpxchg_relaxed((ptr),			\
					_o_, _n_, sizeof(*(ptr)));	\
})

#define __cmpxchg_acquire(ptr, old, new, size)				\
({									\
	__typeof__(ptr) __ptr = (ptr);					\
	__typeof__(*(ptr)) __old = (old);				\
	__typeof__(*(ptr)) __new = (new);				\
	__typeof__(*(ptr)) __ret;					\
	switch (size) {							\
	case 4:								\
		ASM_CMPXCHG(1, 2, 3, w, ,				\
			ASM_ACQUIRE_BARRIER_BLOCK(4), ,			\
			__ret, __new, (long)__old, __ptr);		\
		break;							\
	case 8:								\
		ASM_CMPXCHG(1, 2, 3, d, ,				\
			ASM_ACQUIRE_BARRIER_BLOCK(4), ,			\
			__ret, __new, __old, __ptr);			\
		break;							\
	default:							\
		BUILD_BUG();						\
	}								\
	__ret;								\
})

#define arch_cmpxchg_acquire(ptr, o, n)					\
({									\
	__typeof__(*(ptr)) _o_ = (o);					\
	__typeof__(*(ptr)) _n_ = (n);					\
	(__typeof__(*(ptr))) __cmpxchg_acquire((ptr),			\
					_o_, _n_, sizeof(*(ptr)));	\
})

#define __cmpxchg_release(ptr, old, new, size)				\
({									\
	__typeof__(ptr) __ptr = (ptr);					\
	__typeof__(*(ptr)) __old = (old);				\
	__typeof__(*(ptr)) __new = (new);				\
	__typeof__(*(ptr)) __ret;					\
	switch (size) {							\
	case 4:								\
		ASM_CMPXCHG(1, 2, 3, w,					\
			ASM_RELEASE_BARRIER_BLOCK(4), , ,		\
			__ret, __new, (long)__old, __ptr);		\
		break;							\
	case 8:								\
		ASM_CMPXCHG(1, 2, 3, d,					\
			ASM_RELEASE_BARRIER_BLOCK(4), , ,		\
			__ret, __new, __old, __ptr);			\
		break;							\
	default:							\
		BUILD_BUG();						\
	}								\
	__ret;								\
})

#define arch_cmpxchg_release(ptr, o, n)					\
({									\
	__typeof__(*(ptr)) _o_ = (o);					\
	__typeof__(*(ptr)) _n_ = (n);					\
	(__typeof__(*(ptr))) __cmpxchg_release((ptr),			\
					_o_, _n_, sizeof(*(ptr)));	\
})

#define __cmpxchg(ptr, old, new, size)					\
({									\
	__typeof__(ptr) __ptr = (ptr);					\
	__typeof__(*(ptr)) __old = (old);				\
	__typeof__(*(ptr)) __new = (new);				\
	__typeof__(*(ptr)) __ret;					\
	switch (size) {							\
	case 4:								\
		ASM_CMPXCHG(1, 2, 3, w, ,				\
			ASM_FULL_BARRIER_BLOCK(4), ".rl",		\
			__ret, __new, (long)__old, __ptr);		\
		break;							\
	case 8:								\
		ASM_CMPXCHG(1, 2, 3, d, ,				\
			ASM_FULL_BARRIER_BLOCK(4), ".rl",		\
			__ret, __new, __old, __ptr);			\
		break;							\
	default:							\
		BUILD_BUG();						\
	}								\
	__ret;								\
})

#define arch_cmpxchg(ptr, o, n)						\
({									\
	__typeof__(*(ptr)) _o_ = (o);					\
	__typeof__(*(ptr)) _n_ = (n);					\
	(__typeof__(*(ptr))) __cmpxchg((ptr),				\
				       _o_, _n_, sizeof(*(ptr)));	\
})

#define arch_cmpxchg_local(ptr, o, n)					\
	(__cmpxchg_relaxed((ptr), (o), (n), sizeof(*(ptr))))

#define cmpxchg32(ptr, o, n)						\
({									\
	BUILD_BUG_ON(sizeof(*(ptr)) != 4);				\
	arch_cmpxchg((ptr), (o), (n));					\
})

#define cmpxchg32_local(ptr, o, n)					\
({									\
	BUILD_BUG_ON(sizeof(*(ptr)) != 4);				\
	arch_cmpxchg_relaxed((ptr), (o), (n))				\
})

#define arch_cmpxchg64(ptr, o, n)					\
({									\
	BUILD_BUG_ON(sizeof(*(ptr)) != 8);				\
	arch_cmpxchg((ptr), (o), (n));					\
})

#define arch_cmpxchg64_local(ptr, o, n)					\
({									\
	BUILD_BUG_ON(sizeof(*(ptr)) != 8);				\
	arch_cmpxchg_relaxed((ptr), (o), (n));				\
})

#endif /* _ASM_RISCV_CMPXCHG_H */
