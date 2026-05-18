#ifndef __NUMBER_H__
#define __NUMBER_H__
#include <cstdint>

namespace yat {
uint64_t constexpr sext4(uint64_t a)
{
    return a & 0x8ULL ? a | 0xfffffffffffffff0ULL : a & 0x7ULL;
}

uint64_t constexpr sext5(uint64_t a)
{
    return a & 0x10ULL ? a | 0xffffffffffffffe0ULL : a & 0xfULL;
}

uint64_t constexpr sext8(uint64_t a)
{
    return a & 0x80ULL ? a | 0xffffffffffffff00ULL : a & 0x7fULL;
}

uint64_t constexpr sext16(uint64_t a)
{
    return a & 0x8000ULL ? a | 0xffffffffffff0000ULL : a & 0x7fffULL;
}

uint64_t constexpr sext32(uint64_t a)
{
    return a & 0x80000000ULL ? a | 0xffffffff00000000ULL : a & 0x7fffffffULL;
}

uint64_t constexpr addi(uint64_t a, uint64_t b)
{
    return a + b;
}

uint64_t constexpr addiw(uint64_t a, uint64_t b)
{
    return sext32(a + b);
}

uint64_t constexpr subi(uint64_t a, uint64_t b)
{
    return a - b;
}

uint64_t constexpr subiw(uint64_t a, uint64_t b)
{
    return sext32(a - b);
}

uint64_t constexpr andi_r(uint64_t a, uint64_t b)
{
    return a & sext4(b);
}

uint64_t constexpr andi_t(uint64_t a, uint64_t b)
{
    return a & sext5(b);
}

uint64_t constexpr andiw_r(uint64_t a, uint64_t b)
{
    return sext32(a & sext4(b));
}

uint64_t constexpr andiw_t(uint64_t a, uint64_t b)
{
    return sext32(a & sext5(b));
}

uint64_t constexpr ori_r(uint64_t a, uint64_t b)
{
    return a | sext4(b);
}

uint64_t constexpr ori_t(uint64_t a, uint64_t b)
{
    return a | sext5(b);
}

uint64_t constexpr oriw_r(uint64_t a, uint64_t b)
{
    return sext32(a | sext4(b));
}

uint64_t constexpr oriw_t(uint64_t a, uint64_t b)
{
    return sext32(a | sext5(b));
}

uint64_t constexpr xori_r(uint64_t a, uint64_t b)
{
    return a ^ sext4(b);
}

uint64_t constexpr xori_t(uint64_t a, uint64_t b)
{
    return a ^ sext5(b);
}

uint64_t constexpr xoriw_r(uint64_t a, uint64_t b)
{
    return sext32(a ^ sext4(b));
}

uint64_t constexpr xoriw_t(uint64_t a, uint64_t b)
{
    return sext32(a ^ sext5(b));
}

uint64_t constexpr slli(uint64_t a, uint64_t b)
{
    return a << b;
}

uint64_t constexpr srli(uint64_t a, uint64_t b)
{
    return a >> b;
}

uint64_t constexpr srai(uint64_t a, uint64_t b)
{
    return (int64_t(a) >> b);
}

uint64_t constexpr slliw(uint64_t a, uint64_t b)
{
    return sext32(a << b);
}

uint64_t constexpr srliw(uint64_t a, uint64_t b)
{
    return (a & 0xffffffffULL) >> b;
}

uint64_t constexpr sraiw(uint64_t a, uint64_t b)
{
    return b >= 32 ? \
    (a & 0x80000000ULL ? uint64_t(-1) : 0ULL) \
    : sext32((int32_t(a) >> b));
}

template <uint64_t _Val>
struct Number {
    uint64_t static constexpr Value = _Val;
    Number() = delete;
    Number(Number const &) = delete;
    Number(Number &&) = delete;
};
}
#endif /* __NUMBER_H__ */
