#include <stdint.h>

typedef struct {
    uint16_t base_low;
    uint16_t sel;
    uint8_t  zero;
    uint8_t  flags;
    uint16_t base_high;
} __attribute__((packed)) idt_entry_t;

typedef struct {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed)) idt_ptr_t;

static idt_entry_t idt[256];
static idt_ptr_t   idt_ptr;

#define EXT_ISR(n) extern void isr##n(void);
EXT_ISR(0)  EXT_ISR(1)  EXT_ISR(2)  EXT_ISR(3)  EXT_ISR(4)  EXT_ISR(5)
EXT_ISR(6)  EXT_ISR(7)  EXT_ISR(8)  EXT_ISR(9)  EXT_ISR(10) EXT_ISR(11)
EXT_ISR(12) EXT_ISR(13) EXT_ISR(14) EXT_ISR(15) EXT_ISR(16) EXT_ISR(17)
EXT_ISR(18) EXT_ISR(19) EXT_ISR(20) EXT_ISR(21) EXT_ISR(22) EXT_ISR(23)
EXT_ISR(24) EXT_ISR(25) EXT_ISR(26) EXT_ISR(27) EXT_ISR(28) EXT_ISR(29)
EXT_ISR(30) EXT_ISR(31)

extern void irq0(void);  extern void irq1(void);  extern void irq2(void);
extern void irq3(void);  extern void irq4(void);  extern void irq5(void);
extern void irq6(void);  extern void irq7(void);  extern void irq8(void);
extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void);
extern void irq15(void);

static inline void outb(uint16_t p, uint8_t v){
    __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(p));
}

static void idt_set(int n, uint32_t handler){
    idt[n].base_low  = handler & 0xFFFF;
    idt[n].sel       = 0x08;
    idt[n].zero      = 0;
    idt[n].flags     = 0x8E;
    idt[n].base_high = (handler >> 16) & 0xFFFF;
}

void idt_set_user(int n, uint32_t handler){
    idt[n].base_low  = handler & 0xFFFF;
    idt[n].sel       = 0x08;
    idt[n].zero      = 0;
    idt[n].flags     = 0xEE;   /* present, DPL=3, 32-bit interrupt gate */
    idt[n].base_high = (handler >> 16) & 0xFFFF;
}

static void pic_remap(void){
    outb(0x20, 0x11); outb(0xA0, 0x11);
    outb(0x21, 0x20); outb(0xA1, 0x28);
    outb(0x21, 0x04); outb(0xA1, 0x02);
    outb(0x21, 0x01); outb(0xA1, 0x01);
    outb(0x21, 0xF8);   /* unmask IRQ0,1,2 */
    outb(0xA1, 0xEF);   /* unmask IRQ12 (mouse) */
}

void idt_init(void){
    pic_remap();

    for (int i = 0; i < 256; i++){
        idt[i].base_low = 0; idt[i].base_high = 0;
        idt[i].sel = 0; idt[i].zero = 0; idt[i].flags = 0;
    }

    idt_set(0,  (uint32_t)isr0);  idt_set(1,  (uint32_t)isr1);
    idt_set(2,  (uint32_t)isr2);  idt_set(3,  (uint32_t)isr3);
    idt_set(4,  (uint32_t)isr4);  idt_set(5,  (uint32_t)isr5);
    idt_set(6,  (uint32_t)isr6);  idt_set(7,  (uint32_t)isr7);
    idt_set(8,  (uint32_t)isr8);  idt_set(9,  (uint32_t)isr9);
    idt_set(10, (uint32_t)isr10); idt_set(11, (uint32_t)isr11);
    idt_set(12, (uint32_t)isr12); idt_set(13, (uint32_t)isr13);
    idt_set(14, (uint32_t)isr14); idt_set(15, (uint32_t)isr15);
    idt_set(16, (uint32_t)isr16); idt_set(17, (uint32_t)isr17);
    idt_set(18, (uint32_t)isr18); idt_set(19, (uint32_t)isr19);
    idt_set(20, (uint32_t)isr20); idt_set(21, (uint32_t)isr21);
    idt_set(22, (uint32_t)isr22); idt_set(23, (uint32_t)isr23);
    idt_set(24, (uint32_t)isr24); idt_set(25, (uint32_t)isr25);
    idt_set(26, (uint32_t)isr26); idt_set(27, (uint32_t)isr27);
    idt_set(28, (uint32_t)isr28); idt_set(29, (uint32_t)isr29);
    idt_set(30, (uint32_t)isr30); idt_set(31, (uint32_t)isr31);

    idt_set(32, (uint32_t)irq0);  idt_set(33, (uint32_t)irq1);
    idt_set(34, (uint32_t)irq2);  idt_set(35, (uint32_t)irq3);
    idt_set(36, (uint32_t)irq4);  idt_set(37, (uint32_t)irq5);
    idt_set(38, (uint32_t)irq6);  idt_set(39, (uint32_t)irq7);
    idt_set(40, (uint32_t)irq8);  idt_set(41, (uint32_t)irq9);
    idt_set(42, (uint32_t)irq10); idt_set(43, (uint32_t)irq11);
    idt_set(44, (uint32_t)irq12); idt_set(45, (uint32_t)irq13);
    idt_set(46, (uint32_t)irq14); idt_set(47, (uint32_t)irq15);

    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base  = (uint32_t)&idt;
    __asm__ volatile("lidt %0" :: "m"(idt_ptr));
    __asm__ volatile("sti");
}
