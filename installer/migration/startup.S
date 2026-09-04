/* SPDX-License-Identifier: MIT */

    .syntax unified
    .cpu cortex-m4
    .thumb

    .equ VENDOR_ACCEPTED_STACK, 0x20020000
    .equ PRIVATE_STACK_TOP,      0x20005000

    .section .isr_vector,"a",%progbits
    .align 8
    .global vector_table
vector_table:
    .word VENDOR_ACCEPTED_STACK
    .word Reset_Handler + 1
    .rept 62
    .word Fault_Handler + 1
    .endr

    .section .text.Reset_Handler,"ax",%progbits
    .align 2
    .thumb_func
    .global Reset_Handler
Reset_Handler:
    ldr     r0, =PRIVATE_STACK_TOP
    msr     msp, r0
    bl      migration_main
    movs    r0, #0x7f
    b       migration_recover

    .section .text.Fault_Handler,"ax",%progbits
    .align 2
    .thumb_func
    .global Fault_Handler
Fault_Handler:
    movs    r0, #0x7e
    b       migration_recover

    .section .text.call_mbr,"ax",%progbits
    .align 2
    .thumb_func
    .global call_mbr
call_mbr:
    svc     0x18
    bx      lr
