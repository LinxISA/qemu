#ifndef _ATOMIC_H_
#define _ATOMIC_H_

#include "cmpxchg.h"

typedef long long	s64;

typedef struct {
	int counter;
} atomic_t;

typedef struct {
	s64 counter;
} atomic64_t;

#define PUSH_BLOCK_TEXT_BODY_SECTION ".pushsection \".text.body\",\"ax\"\n"
#define POP_BLOCK_TEXT_BODY_SECTION  ".popsection\n"

////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////

#define ATOMIC_OP(op, asm_op, I, asm_type, c_type, prefix)		\
static inline							\
void arch_atomic##prefix##_##op(c_type i, atomic##prefix##_t *v)	\
{									\
	c_type *p_counter = &(v->counter);				\
	__asm__ __volatile__ (						\
		"bstart 1f\n"						\
		"bnext.fall\n"						\
		"bget %0, %1\n"						\
		"battr atomic\n"					\
		"b.std\n"						\
		"bstop 2f\n"						\
		PUSH_BLOCK_TEXT_BODY_SECTION				\
		"1:\n"							\
			"get %0\n"					\
			"l" #asm_type " [t#1, 0]\n"			\
			"get %1\n"					\
			"" #asm_op " t#1, t#2\n"			\
			"s" #asm_type " t#1, [t#4, 0]\n"		\
		"2:\n"							\
		POP_BLOCK_TEXT_BODY_SECTION				\
		: "+r" (p_counter)					\
		: "r" (I)						\
		: "memory");						\
}

#ifdef CONFIG_GENERIC_ATOMIC64
#define ATOMIC_OPS(op, asm_op, I)					\
        ATOMIC_OP (op, asm_op, I, w, int,   )
#else
#define ATOMIC_OPS(op, asm_op, I)					\
        ATOMIC_OP (op, asm_op, I, w, int,   )				\
        ATOMIC_OP (op, asm_op, I, d, s64, 64)
#endif

//无返回值
//arch_atomic[64]_add
ATOMIC_OPS(add, add,  i)
//arch_atomic[64]_sub
ATOMIC_OPS(sub, add, -i)
//arch_atomic[64]_and
ATOMIC_OPS(and, and,  i)
//arch_atomic[64]_or
ATOMIC_OPS( or,  or,  i)
//arch_atomic[64]_xor
ATOMIC_OPS(xor, xor,  i)

////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////

#undef ATOMIC_OP
#undef ATOMIC_OPS

#define ATOMIC_FETCH_OP(op, asm_op, I, asm_type, c_type, prefix)		\
static inline								\
c_type arch_atomic##prefix##_fetch_##op##_relaxed(c_type i,			\
					     atomic##prefix##_t *v)		\
{										\
	register c_type ret;							\
	c_type *p_counter = &(v->counter);					\
	__asm__ __volatile__ (							\
		"bstart 1f\n"							\
		"bnext.fall\n"							\
		"bget %0, %2\n"							\
		"bset %1\n"							\
		"battr atomic\n"						\
		"b.std\n"							\
		"bstop 2f\n"							\
		PUSH_BLOCK_TEXT_BODY_SECTION					\
		"1:\n"								\
			"get %0\n"						\
			"l" #asm_type " [t#1, 0]\n"				\
			"get %2\n"						\
			"" #asm_op " t#1, t#2\n"				\
			"s" #asm_type " t#1, [t#4, 0]\n"			\
			"set %1, t#4\n"						\
		"2:\n"								\
		POP_BLOCK_TEXT_BODY_SECTION					\
		: "+r" (p_counter), "=r" (ret)					\
		: "r" (I)							\
		: "memory");							\
	return ret;								\
}										\
static inline								\
c_type arch_atomic##prefix##_fetch_##op(c_type i, atomic##prefix##_t *v)	\
{										\
	register c_type ret;							\
	c_type *p_counter = &(v->counter);					\
	__asm__ __volatile__ (							\
		"bstart 1f\n"							\
		"bnext.fall\n"							\
		"bget %0, %2\n"							\
		"bset %1\n"							\
		"battr atomic.aqrl\n"						\
		"b.std\n"							\
		"bstop 2f\n"							\
		PUSH_BLOCK_TEXT_BODY_SECTION					\
		"1:\n"								\
			"get %0\n"						\
			"l" #asm_type " [t#1, 0]\n"				\
			"get %2\n"						\
			"" #asm_op " t#1, t#2\n"				\
			"s" #asm_type " t#1, [t#4, 0]\n"			\
			"set %1, t#4\n"						\
		"2:\n"								\
		POP_BLOCK_TEXT_BODY_SECTION					\
		: "+r" (p_counter), "=r" (ret)					\
		: "r" (I)							\
		: "memory");							\
	return ret;								\
}

#define ATOMIC_OP_RETURN(op, asm_op, c_op, I, asm_type, c_type, prefix)	\
static inline							\
c_type arch_atomic##prefix##_##op##_return_relaxed(c_type i,		\
					      atomic##prefix##_t *v)	\
{									\
        return arch_atomic##prefix##_fetch_##op##_relaxed(i, v) c_op I;	\
}									\
static inline							\
c_type arch_atomic##prefix##_##op##_return(c_type i, atomic##prefix##_t *v)	\
{									\
        return arch_atomic##prefix##_fetch_##op(i, v) c_op I;		\
}

#ifdef CONFIG_GENERIC_ATOMIC64
#define ATOMIC_OPS(op, asm_op, c_op, I)					\
        ATOMIC_FETCH_OP( op, asm_op,       I, w, int,   )		\
        ATOMIC_OP_RETURN(op, asm_op, c_op, I, w, int,   )
#else
#define ATOMIC_OPS(op, asm_op, c_op, I)					\
        ATOMIC_FETCH_OP( op, asm_op,       I, w, int,   )		\
        ATOMIC_OP_RETURN(op, asm_op, c_op, I, w, int,   )		\
        ATOMIC_FETCH_OP( op, asm_op,       I, d, s64, 64)		\
        ATOMIC_OP_RETURN(op, asm_op, c_op, I, d, s64, 64)
#endif

//fetch_op   op操作，返回op之前的值
//op_return  op操作，返回op之后的值
ATOMIC_OPS(add, add, +,  i)
ATOMIC_OPS(sub, add, +, -i)

#define arch_atomic_add_return_relaxed	arch_atomic_add_return_relaxed
#define arch_atomic_sub_return_relaxed	arch_atomic_sub_return_relaxed
#define arch_atomic_add_return		arch_atomic_add_return
#define arch_atomic_sub_return		arch_atomic_sub_return

#define arch_atomic_fetch_add_relaxed	arch_atomic_fetch_add_relaxed
#define arch_atomic_fetch_sub_relaxed	arch_atomic_fetch_sub_relaxed
#define arch_atomic_fetch_add		arch_atomic_fetch_add
#define arch_atomic_fetch_sub		arch_atomic_fetch_sub

#ifndef CONFIG_GENERIC_ATOMIC64
#define arch_atomic64_add_return_relaxed	arch_atomic64_add_return_relaxed
#define arch_atomic64_sub_return_relaxed	arch_atomic64_sub_return_relaxed
#define arch_atomic64_add_return		arch_atomic64_add_return
#define arch_atomic64_sub_return		arch_atomic64_sub_return

#define arch_atomic64_fetch_add_relaxed	arch_atomic64_fetch_add_relaxed
#define arch_atomic64_fetch_sub_relaxed	arch_atomic64_fetch_sub_relaxed
#define arch_atomic64_fetch_add		arch_atomic64_fetch_add
#define arch_atomic64_fetch_sub		arch_atomic64_fetch_sub
#endif

////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////

#undef ATOMIC_OPS

//跟上面相比，少了op_return操作
#ifdef CONFIG_GENERIC_ATOMIC64
#define ATOMIC_OPS(op, asm_op, I)					\
        ATOMIC_FETCH_OP(op, asm_op, I, w, int,   )
#else
#define ATOMIC_OPS(op, asm_op, I)					\
        ATOMIC_FETCH_OP(op, asm_op, I, w, int,   )			\
        ATOMIC_FETCH_OP(op, asm_op, I, d, s64, 64)
#endif

ATOMIC_OPS(and, and, i)
ATOMIC_OPS( or,  or, i)
ATOMIC_OPS(xor, xor, i)

#define arch_atomic_fetch_and_relaxed	arch_atomic_fetch_and_relaxed
#define arch_atomic_fetch_or_relaxed	arch_atomic_fetch_or_relaxed
#define arch_atomic_fetch_xor_relaxed	arch_atomic_fetch_xor_relaxed
#define arch_atomic_fetch_and		arch_atomic_fetch_and
#define arch_atomic_fetch_or		arch_atomic_fetch_or
#define arch_atomic_fetch_xor		arch_atomic_fetch_xor

#ifndef CONFIG_GENERIC_ATOMIC64
#define arch_atomic64_fetch_and_relaxed	arch_atomic64_fetch_and_relaxed
#define arch_atomic64_fetch_or_relaxed	arch_atomic64_fetch_or_relaxed
#define arch_atomic64_fetch_xor_relaxed	arch_atomic64_fetch_xor_relaxed
#define arch_atomic64_fetch_and		arch_atomic64_fetch_and
#define arch_atomic64_fetch_or		arch_atomic64_fetch_or
#define arch_atomic64_fetch_xor		arch_atomic64_fetch_xor
#endif

////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////

#undef ATOMIC_OPS

#undef ATOMIC_FETCH_OP
#undef ATOMIC_OP_RETURN

#define ASM_ATOMIC_FETCH_ADD_UNLESS(type, prev, ptr, a, u)	\
	__asm__ __volatile__ (					\
		"1:					\n"	\
		"bstart 10f				\n"	\
		"b.sys					\n"	\
		"battr atomic				\n"	\
		"bget %[ptr], %[u]			\n"	\
		"bset %[prev]				\n"	\
		"bnext.cond 4f				\n"	\
		"bstop 11f				\n"	\
		PUSH_BLOCK_TEXT_BODY_SECTION			\
		"10:					\n"	\
			"get %[ptr]			\n"	\
			"lr." #type " [t#1]		\n"	\
			"set %[prev], t#1		\n"	\
			"get %[u]			\n"	\
			"setc.eq t#1, t#3		\n"	\
		"11:					\n"	\
		POP_BLOCK_TEXT_BODY_SECTION			\
		"2:					\n"	\
		"bstart 20f				\n"	\
		"b.sys					\n"	\
		"battr atomic.rl			\n"	\
		"bget %[prev], %[a], %[ptr]		\n"	\
		"bnext.cond 1b				\n"	\
		"bstop 21f				\n"	\
		PUSH_BLOCK_TEXT_BODY_SECTION			\
		"20:					\n"	\
			"get %[prev]			\n"	\
			"get %[a]			\n"	\
			"add t#1, t#2			\n"	\
			"get %[ptr]			\n"	\
			"sc." #type " t#2, [t#1]	\n"	\
			"setc.nei t#1, 0		\n"	\
		"21:					\n"	\
		POP_BLOCK_TEXT_BODY_SECTION			\
		ASM_FULL_BARRIER_BLOCK(3)			\
		"4:					\n"	\
		: [prev] "=&r" (prev)				\
		: [ptr] "r" (ptr), [a] "r" (a), [u] "r" (u)	\
		: "memory")

/* This is required to provide a full barrier on success. */
static inline int arch_atomic_fetch_add_unless(atomic_t *v, int a, int u)
{
	int prev;
	__typeof__(&v->counter) ptr = &v->counter;

	ASM_ATOMIC_FETCH_ADD_UNLESS(w, prev, ptr, a, u);

	return prev;
}
#define arch_atomic_fetch_add_unless arch_atomic_fetch_add_unless

#ifndef CONFIG_GENERIC_ATOMIC64
static inline s64 arch_atomic64_fetch_add_unless(atomic64_t *v, s64 a, s64 u)
{
	s64 prev;
	__typeof__(&v->counter) ptr = &v->counter;

	ASM_ATOMIC_FETCH_ADD_UNLESS(d, prev, ptr, a, u);

	return prev;
}
#define arch_atomic64_fetch_add_unless arch_atomic64_fetch_add_unless
#endif

/*
 * atomic_{cmp,}xchg is required to have exactly the same ordering semantics as
 * {cmp,}xchg and the operations that return, so they need a full barrier.
 */
#define ATOMIC_OP(c_t, prefix, size)					\
static inline							\
c_t arch_atomic##prefix##_xchg_relaxed(atomic##prefix##_t *v, c_t n)	\
{									\
	return __xchg_relaxed(&(v->counter), n, size);			\
}									\
static inline							\
c_t arch_atomic##prefix##_xchg_acquire(atomic##prefix##_t *v, c_t n)	\
{									\
	return __xchg_acquire(&(v->counter), n, size);			\
}									\
static inline							\
c_t arch_atomic##prefix##_xchg_release(atomic##prefix##_t *v, c_t n)	\
{									\
	return __xchg_release(&(v->counter), n, size);			\
}									\
static inline							\
c_t arch_atomic##prefix##_xchg(atomic##prefix##_t *v, c_t n)		\
{									\
	return __xchg(&(v->counter), n, size);				\
}									\
static inline							\
c_t arch_atomic##prefix##_cmpxchg_relaxed(atomic##prefix##_t *v,	\
				     c_t o, c_t n)			\
{									\
	return __cmpxchg_relaxed(&(v->counter), o, n, size);		\
}									\
static inline							\
c_t arch_atomic##prefix##_cmpxchg_acquire(atomic##prefix##_t *v,	\
				     c_t o, c_t n)			\
{									\
	return __cmpxchg_acquire(&(v->counter), o, n, size);		\
}									\
static inline							\
c_t arch_atomic##prefix##_cmpxchg_release(atomic##prefix##_t *v,	\
				     c_t o, c_t n)			\
{									\
	return __cmpxchg_release(&(v->counter), o, n, size);		\
}									\
static inline							\
c_t arch_atomic##prefix##_cmpxchg(atomic##prefix##_t *v, c_t o, c_t n)	\
{									\
	return __cmpxchg(&(v->counter), o, n, size);			\
}

#ifdef CONFIG_GENERIC_ATOMIC64
#define ATOMIC_OPS()							\
	ATOMIC_OP(int,   , 4)
#else
#define ATOMIC_OPS()							\
	ATOMIC_OP(int,   , 4)						\
	ATOMIC_OP(s64, 64, 8)
#endif

ATOMIC_OPS()

#define arch_atomic64_xchg_relaxed	arch_atomic64_xchg_relaxed
#define arch_atomic64_xchg_acquire	arch_atomic64_xchg_acquire
#define arch_atomic64_xchg_release	arch_atomic64_xchg_release
#define arch_atomic64_xchg		arch_atomic64_xchg
#define arch_atomic64_cmpxchg_relaxed	arch_atomic64_cmpxchg_relaxed
#define arch_atomic64_cmpxchg_acquire	arch_atomic64_cmpxchg_acquire
#define arch_atomic64_cmpxchg_release	arch_atomic64_cmpxchg_release
#define arch_atomic64_cmpxchg		arch_atomic64_cmpxchg

#define arch_atomic_xchg_relaxed	arch_atomic_xchg_relaxed
#define arch_atomic_xchg_acquire	arch_atomic_xchg_acquire
#define arch_atomic_xchg_release	arch_atomic_xchg_release
#define arch_atomic_xchg		arch_atomic_xchg
#define arch_atomic_cmpxchg_relaxed	arch_atomic_cmpxchg_relaxed
#define arch_atomic_cmpxchg_acquire	arch_atomic_cmpxchg_acquire
#define arch_atomic_cmpxchg_release	arch_atomic_cmpxchg_release
#define arch_atomic_cmpxchg		arch_atomic_cmpxchg

#undef ATOMIC_OPS
#undef ATOMIC_OP

#define ASM_ATOMIC_SUB_IF_POSITIVE(type, ret, ptr, offset)	\
	__asm__ __volatile__ (					\
		"1:					\n"	\
		"bstart 10f				\n"	\
		"b.sys					\n"	\
		"battr atomic				\n"	\
		"bget %[ptr], %[offset]			\n"	\
		"bset %[ret]				\n"	\
		"bnext.cond 4f				\n"	\
		"bstop 11f				\n"	\
		PUSH_BLOCK_TEXT_BODY_SECTION			\
		"10:					\n"	\
			"get %[ptr]			\n"	\
			"lr." #type " [t#1]		\n"	\
			"get %[offset]			\n"	\
			"sub t#2, t#1			\n"	\
			"set %[ret], t#1		\n"	\
			"setc.lti t#2, 0		\n"	\
		"11:					\n"	\
		POP_BLOCK_TEXT_BODY_SECTION			\
		"2:					\n"	\
		"bstart 20f				\n"	\
		"b.sys					\n"	\
		"battr atomic.rl			\n"	\
		"bget %[ret], %[ptr]			\n"	\
		"bnext.cond 1b				\n"	\
		"bstop 21f				\n"	\
		PUSH_BLOCK_TEXT_BODY_SECTION			\
		"20:					\n"	\
			"get %[ret]			\n"	\
			"get %[ptr]			\n"	\
			"sc." #type " t#2, [t#1]	\n"	\
			"setc.nei t#1, 0		\n"	\
		"21:					\n"	\
		POP_BLOCK_TEXT_BODY_SECTION			\
		ASM_FULL_BARRIER_BLOCK(3)			\
		"4:					\n"	\
		: [ret] "=&r" (ret)				\
		: [ptr] "r" (ptr), [offset] "r" (offset)	\
		: "memory")

static inline int arch_atomic_sub_if_positive(atomic_t *v, int offset)
{
	int ret;
	__typeof__(&v->counter) ptr = &v->counter;

	ASM_ATOMIC_SUB_IF_POSITIVE(w, ret, ptr, offset);

	return ret;
}

#define arch_atomic_dec_if_positive(v)	arch_atomic_sub_if_positive(v, 1)

#ifndef CONFIG_GENERIC_ATOMIC64
static inline s64 arch_atomic64_sub_if_positive(atomic64_t *v, s64 offset)
{
	s64 ret;
	__typeof__(&v->counter) ptr = &v->counter;

	ASM_ATOMIC_SUB_IF_POSITIVE(d, ret, ptr, offset);

	return ret;
}

#define arch_atomic64_dec_if_positive(v)	arch_atomic64_sub_if_positive(v, 1)
#endif

#endif /* _ASM_RISCV_ATOMIC_H */
