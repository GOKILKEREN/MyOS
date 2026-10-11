/* gdt.c - Global Descriptor Table + TSS untuk MyOS */
#include <stdint.h>

/* GDT entry: 8 byte */
typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  gran;
    uint8_t  base_high;
} __attribute__((packed)) gdt_entry_t;

typedef struct {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed)) gdt_ptr_t;

/* TSS 32-bit (104 bytes) */
typedef struct {
    uint32_t prev_tss;
    uint32_t esp0;
    uint32_t ss0;
    uint32_t esp1;
    uint32_t ss1;
    uint32_t esp2;
    uint32_t ss2;
    uint32_t cr3;
    uint32_t eip;
    uint32_t eflags;
    uint32_t eax, ecx, edx, ebx;
    uint32_t esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs;
    uint32_t ldt;
    uint16_t trap;
    uint16_t iomap_base;
} __attribute__((packed)) tss_t;

static gdt_entry_t gdt[6];
static gdt_ptr_t   gdtr;
static tss_t       tss;

/* Kernel stack buat ring 0 saat interrupt dari ring 3 */
static uint8_t kernel_stack[16384] __attribute__((aligned(16)));

/* Set GDT entry biasa (code/data) */
static void gdt_set(int i, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran){
    gdt[i].base_low  = base & 0xFFFF;
    gdt[i].base_mid  = (base >> 16) & 0xFF;
    gdt[i].base_high = (base >> 24) & 0xFF;
    gdt[i].limit_low = limit & 0xFFFF;
    gdt[i].gran      = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt[i].access    = access;
}

/* Set GDT entry TSS (system descriptor) */
static void gdt_set_tss(int i, uint32_t base, uint32_t limit){
    gdt[i].base_low  = base & 0xFFFF;
    gdt[i].base_mid  = (base >> 16) & 0xFF;
    gdt[i].base_high = (base >> 24) & 0xFF;
    gdt[i].limit_low = limit & 0xFFFF;
    gdt[i].gran      = (limit >> 16) & 0x0F;
    gdt[i].access    = 0x89;   /* present, ring0, 32-bit TSS available */
}

void tss_set_kernel_stack(uint32_t esp0){
    tss.esp0 = esp0;
}

void gdt_init(void){
    /* 0: null */
    gdt_set(0, 0, 0, 0, 0);

    /* 1: kernel code — 0x08 */
    gdt_set(1, 0, 0xFFFFF, 0x9A, 0xCF);

    /* 2: kernel data — 0x10 */
    gdt_set(2, 0, 0xFFFFF, 0x92, 0xCF);

    /* 3: user code — 0x18 (DPL=3) */
    gdt_set(3, 0, 0xFFFFF, 0xFA, 0xCF);

    /* 4: user data — 0x20 (DPL=3) */
    gdt_set(4, 0, 0xFFFFF, 0xF2, 0xCF);

    /* 5: TSS — 0x28 */
    for(uint32_t i = 0; i < sizeof(tss); i++) ((uint8_t*)&tss)[i] = 0;
    tss.ss0      = 0x10;                                   /* kernel data segment */
    tss.esp0     = (uint32_t)(kernel_stack + sizeof(kernel_stack));
    tss.iomap_base = sizeof(tss);
    gdt_set_tss(5, (uint32_t)&tss, sizeof(tss) - 1);

    gdtr.limit = sizeof(gdt) - 1;
    gdtr.base  = (uint32_t)&gdt;

    /* Load GDT */
    __asm__ volatile("lgdt %0" :: "m"(gdtr));

    /* Reload segment register: CS via far jump */
    __asm__ volatile(
        "ljmp $0x08, $1f\n"
        "1:\n"
        "mov $0x10, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "mov %%ax, %%ss\n"
        ::: "eax", "memory");

    /* Load Task Register dengan TSS selector 0x28 */
    __asm__ volatile("ltr %0" :: "r"((uint16_t)0x28));
}
