#include <stdint.h>

extern void task_spawn_user(uint32_t entry, uint32_t user_stack_top);

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf32_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} Elf32_Phdr;

#define PT_LOAD      1
#define ET_EXEC      2
#define EM_386       3

#define USER_STACK_TOP 0x3F00000u

int elf_exec(const uint8_t* elf, uint32_t size){
    if(size < sizeof(Elf32_Ehdr)) return -1;
    if(elf[0] != 0x7F || elf[1] != 'E' || elf[2] != 'L' || elf[3] != 'F') return -2;

    const Elf32_Ehdr* hdr = (const Elf32_Ehdr*)elf;
    if(hdr->e_machine != EM_386) return -3;
    if(hdr->e_type    != ET_EXEC) return -4;
    if(hdr->e_phentsize != sizeof(Elf32_Phdr)) return -5;

    for(int i = 0; i < hdr->e_phnum; i++){
        const Elf32_Phdr* ph = (const Elf32_Phdr*)(elf + hdr->e_phoff + i*hdr->e_phentsize);
        if(ph->p_type != PT_LOAD) continue;
        if(ph->p_offset + ph->p_filesz > size) return -6;

        uint8_t* dst = (uint8_t*)(uintptr_t)ph->p_vaddr;
        const uint8_t* src = elf + ph->p_offset;
        for(uint32_t j = 0; j < ph->p_filesz; j++) dst[j] = src[j];
        for(uint32_t j = ph->p_filesz; j < ph->p_memsz; j++) dst[j] = 0;
    }

    task_spawn_user(hdr->e_entry, USER_STACK_TOP);
    return 0;
}
