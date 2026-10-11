// Copyright (c) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)

.section .text._start,"ax",@progbits
.global _start
.type _start,%function
.p2align 2
_start:
    mov x29, xzr
    mov x30, xzr
    ldr x0, [sp]
    add x1, sp, #8
    bl main
    bl exit
    brk #0
.size _start, .-_start

.section .note.GNU-stack,"",@progbits
