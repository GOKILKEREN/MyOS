#ifndef USER_LIB_H
#define USER_LIB_H

typedef unsigned int   uint32_t;
typedef unsigned char  uint8_t;
typedef unsigned short uint16_t;

/* ===== syscall wrappers =====
   EAX di-set eksplisit di dalam asm body (bukan constraint)
   supaya GCC gak bisa nge-optimize out. EBX di push/pop karena
   callee-saved di C ABI. */

static inline void sys_print(const char* s){
    __asm__ volatile(
        "pushl %%ebx\n\t"
        "movl $1, %%eax\n\t"
        "movl %0, %%ebx\n\t"
        "int $0x80\n\t"
        "popl %%ebx"
        :
        : "r"((uint32_t)s)
        : "eax", "ecx", "edx", "memory");
}

static inline void sys_exit(void){
    __asm__ volatile(
        "pushl %%ebx\n\t"
        "movl $0, %%eax\n\t"
        "int $0x80\n\t"
        "popl %%ebx"
        :
        :
        : "eax", "ecx", "edx", "memory");
    for(;;);
}

static inline int sys_getch(void){
    uint32_t r;
    __asm__ volatile(
        "pushl %%ebx\n\t"
        "movl $2, %%eax\n\t"
        "int $0x80\n\t"
        "popl %%ebx"
        : "=a"(r)
        :
        : "ecx", "edx", "memory");
    return (int)r;
}

static inline void sys_yield(void){
    __asm__ volatile(
        "pushl %%ebx\n\t"
        "movl $3, %%eax\n\t"
        "int $0x80\n\t"
        "popl %%ebx"
        :
        :
        : "eax", "ecx", "edx", "memory");
}

/* ===== string helpers ===== */
static inline int strlen_(const char* s){ int n = 0; while(s[n]) n++; return n; }
static inline void puts_(const char* s){ sys_print(s); }
static inline void putc_(char c){ char b[2] = {c, 0}; sys_print(b); }
static inline void endl(void){ sys_print("\n"); }

static inline void put_int(int v){
    char buf[16]; int i = 0;
    if(v == 0) buf[i++] = '0';
    else {
        int neg = 0;
        if(v < 0){ neg = 1; v = -v; }
        char tmp[12]; int j = 0;
        while(v){ tmp[j++] = '0' + (v % 10); v /= 10; }
        if(neg) buf[i++] = '-';
        while(j) buf[i++] = tmp[--j];
    }
    buf[i] = 0;
    sys_print(buf);
}
static inline void put_intln(int v){ put_int(v); endl(); }

static inline void banner(const char* name){
    sys_print("=== ");
    sys_print(name);
    sys_print(" ===\n");
}

#endif
