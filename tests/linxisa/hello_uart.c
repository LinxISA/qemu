#include <stdint.h>

#define LINX_VIRT_UART_BASE 0x10000000u
#define LINX_VIRT_FINISHER_BASE 0x10009000u

static inline uint32_t mmio_read32(uint32_t addr)
{
    return *(volatile uint32_t *)(uintptr_t)addr;
}

static inline void mmio_write32(uint32_t addr, uint32_t value)
{
    *(volatile uint32_t *)(uintptr_t)addr = value;
}

static inline void uart_putc(char c)
{
    /* Status is readable at UART+0x4; bit0 indicates TX ready (always 1 today). */
    while ((mmio_read32(LINX_VIRT_UART_BASE + 0x4) & 1u) == 0) {
    }
    mmio_write32(LINX_VIRT_UART_BASE + 0x0, (uint32_t)(uint8_t)c);
}

static void uart_puts(const char *s)
{
    for (; *s; s++) {
        if (*s == '\n') {
            uart_putc('\r');
        }
        uart_putc(*s);
    }
}

static __attribute__((noreturn)) void virt_exit(uint32_t code)
{
    const uint32_t value = code == 0 ? 0x5555u : 0x3333u;

    mmio_write32(LINX_VIRT_FINISHER_BASE, value);
    for (;;) {
    }
}

int _start(void)
{
    uart_puts("hello from LinxISA QEMU virt UART\n");
    virt_exit(0);
}
