BITS 32

section .multiboot
align 4
    dd 0x1BADB002              ; magic
    dd 0x00000004              ; flags: video mode request
    dd -(0x1BADB002 + 0x00000004) ; checksum
    dd 0                       ; header_addr  (unused for ELF)
    dd 0                       ; load_addr    (unused for ELF)
    dd 0                       ; load_end_addr(unused for ELF)
    dd 0                       ; bss_end_addr (unused for ELF)
    dd 0                       ; entry_addr   (unused for ELF)
    dd 0                       ; mode_type: 0 = linear graphics
    dd 800                     ; width
    dd 600                     ; height
    dd 32                      ; depth

section .bss
align 16
stack_bottom:
    resb 16384
stack_top:

section .text
global _start
extern kernel_main

_start:
    cli
    cld
    mov esp, stack_top
    push ebx
    push eax
    call kernel_main
.hang:
    cli
    hlt
    jmp .hang
