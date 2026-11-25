/* main 的链接包装入口：此时已在用户态，sp 在栈顶，尚未建立 C 栈帧。 */
    .section .text.__wrap_main,"ax",@progbits
    .global __wrap_main
    .type __wrap_main,@function
__wrap_main:
    /* 与 SDK __risc_v_disable_irq 一致，使用用户态 CSR 0x800。 */
    li t0, 0x88
    csrrc t3, 0x800, t0
    fence.i
    la t0, _susrstack
    la t1, _eusrstack
    li t2, 0xa5a5a5a5
1:
    sw t2, 0(t0)
    addi t0, t0, 4
    bltu t0, t1, 1b
    andi t3, t3, 0x88
    csrs 0x800, t3
    tail __real_main
    .size __wrap_main, .-__wrap_main
