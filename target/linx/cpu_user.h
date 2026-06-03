#ifndef TARGET_LINX_CPU_USER_H
#define TARGET_LINX_CPU_USER_H

#define xRA 10   /* return address (aka link register) */
#define xSP 1   /* stack pointer */

#define xA0 2  /* gpr[2-8] are syscall arguments */
#define xA1 3
#define xA2 4
#define xA3 5
#define xA4 6
#define xA5 7
#define xA6 8
#define xA7 9
#define xX1 21  /* syscall number */

#endif
