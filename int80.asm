BITS 32
section .text

extern syscall_handler

global isr128
isr128:
    push dword 0        ; err_code dummy
    push dword 0x80     ; int_no
    pusha
    cld
    push esp            ; pointer ke regs_t
    call syscall_handler
    add esp, 4
    popa
    add esp, 8
    iret
