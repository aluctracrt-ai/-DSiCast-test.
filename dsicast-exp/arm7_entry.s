.syntax unified
.arm
.section .init, "ax", %progbits
.global _start
.type _start, %function
_start:
    mov r3, sp
    ldr sp, =0x02FD8000
    stmdb sp!, {r3, lr}
    bl dsicast7_tick_impl
    ldmia sp!, {r3, lr}
    mov sp, r3
    bx lr
.size _start, .-_start
