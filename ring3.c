#include <stdint.h>

extern void user_prog_entry(void);

static uint8_t user_stack[8192] __attribute__((aligned(16)));

void enter_user_at(uint32_t entry, uint32_t user_stack_top){
    __asm__ volatile(
        "cli\n"
        "pushl $0x23\n"
        "pushl %0\n"
        "pushfl\n"
        "orl $0x200, (%%esp)\n"
        "pushl $0x1B\n"
        "pushl %1\n"
        "iret\n"
        :: "r"(user_stack_top), "r"(entry)
        : "memory");
    for(;;) __asm__ volatile("hlt");
}

void enter_user_mode(void){
    uint32_t stack_top = (uint32_t)(user_stack + sizeof(user_stack));
    __asm__ volatile(
        "cli\n"
        "pushl $0x23\n"
        "pushl %0\n"
        "pushfl\n"
        "orl $0x200, (%%esp)\n"
        "pushl $0x1B\n"
        "pushl %1\n"
        "iret\n"
        :: "r"(stack_top), "r"((uint32_t)user_prog_entry)
        : "memory");
    for(;;) __asm__ volatile("hlt");
}
