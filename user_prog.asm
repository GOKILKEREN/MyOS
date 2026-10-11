BITS 32
section .text
global user_prog_entry
global user_msg

user_prog_entry:
    ; Set data segments ke user data (boleh di sini, CPL=3)
    mov ax, 0x23
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; SYS_PRINT(user_msg)
    mov eax, 1
    mov ebx, user_msg
    int 0x80

    ; SYS_EXIT
    mov eax, 0
    int 0x80

    jmp $

section .rodata
user_msg:
    db "[ring3] Hello from userspace (CPL=3)!", 10, 0
