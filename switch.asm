BITS 32
section .text

global task_switch_to

; void task_switch_to(uint32_t* save_esp_ptr, uint32_t new_esp);
; Stack saat entry: [ret][save_esp_ptr][new_esp]
task_switch_to:
    pushfd                  ; EFLAGS
    push dword 0x08         ; CS = kernel code
    push dword .resume      ; EIP
    push dword 0            ; err_code
    push dword 0            ; int_no
    pusha
    cld
    mov eax, [esp + 56]     ; save_esp_ptr
    mov edx, [esp + 60]     ; new_esp
    mov [eax], esp
    mov esp, edx
    popa
    add esp, 8
    iret
.resume:
    ret
