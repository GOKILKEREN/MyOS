/* paging.c - identity mapping dengan 4 MB pages (PSE) */
#include <stdint.h>

#define PDE_PRESENT  0x01
#define PDE_RW       0x02
#define PDE_PS       0x80
#define PDE_USER     0x04
#define PDE_ATTR     (PDE_PRESENT | PDE_RW | PDE_PS | PDE_USER)

/* 1024 entri × 4 byte = 4 KB, harus aligned 4 KB */
static uint32_t page_dir[1024] __attribute__((aligned(4096)));

void paging_init(uint32_t fb_addr){
    /* Identity map 0-32 MB (8 × 4 MB pages) untuk kernel + bss */
    for(int i = 0; i < 16; i++)
        page_dir[i] = (uint32_t)(i * 0x400000) | PDE_ATTR;

    /* Identity map region framebuffer (4 MB page tunggal di sekitar fb_addr) */
    uint32_t fb_pde = (fb_addr >> 22) & 0x3FF;
    page_dir[fb_pde] = (fb_pde << 22) | PDE_ATTR;

    /* Load page directory ke CR3 */
    __asm__ volatile("mov %0, %%cr3" :: "r"(page_dir) : "memory");

    /* Aktifkan PSE (4 MB page) di CR4, lalu PG di CR0 */
    uint32_t cr4, cr0;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= 0x10;
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4));

    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000u;
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0) : "memory");
}
