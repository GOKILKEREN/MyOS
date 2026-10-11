#ifndef USER_GUI_H
#define USER_GUI_H
#include "lib.h"

#define EV_NONE  0
#define EV_MOUSE 1
#define EV_KEY   2
#define EV_CLOSE 3

#define COLOR_BLACK   0x000000
#define COLOR_WHITE   0xFFFFFF
#define COLOR_RED     0xC04040
#define COLOR_GREEN   0x20A040
#define COLOR_BLUE    0x3A6EA5
#define COLOR_YELLOW  0xD0A040
#define COLOR_GRAY    0x808080
#define COLOR_DARK    0x101018
#define COLOR_LIGHT   0xF0F0F0

typedef struct {
    int type;
    int x, y;
    int btn;
    int key;
} gui_event_t;

typedef struct {
    int x, y, w, h;
    char title[32];
} gui_win_req_t;

typedef struct {
    int x, y, w, h;
    uint32_t color;
} gui_rect_req_t;

typedef struct {
    int x, y;
    uint32_t color;
    const char* text;
} gui_text_req_t;

/* syscalls */
static inline void gui_create(const char* title, int x, int y, int w, int h){
    gui_win_req_t req;
    req.x = x; req.y = y; req.w = w; req.h = h;
    int i = 0; while(title[i] && i < 31){ req.title[i] = title[i]; i++; }
    req.title[i] = 0;
    __asm__ volatile(
        "pushl %%ebx\n\t"
        "movl $10, %%eax\n\t"
        "movl %0, %%ebx\n\t"
        "int $0x80\n\t"
        "popl %%ebx"
        :: "r"((uint32_t)&req) : "eax", "ecx", "edx", "memory");
}
static inline void gui_clear(uint32_t color){
    __asm__ volatile(
        "pushl %%ebx\n\t"
        "movl $16, %%eax\n\t"
        "movl %0, %%ebx\n\t"
        "int $0x80\n\t"
        "popl %%ebx"
        :: "r"(color) : "eax", "ecx", "edx", "memory");
}
static inline void gui_rect(int x, int y, int w, int h, uint32_t color){
    gui_rect_req_t r; r.x = x; r.y = y; r.w = w; r.h = h; r.color = color;
    __asm__ volatile(
        "pushl %%ebx\n\t"
        "movl $13, %%eax\n\t"
        "movl %0, %%ebx\n\t"
        "int $0x80\n\t"
        "popl %%ebx"
        :: "r"((uint32_t)&r) : "eax", "ecx", "edx", "memory");
}
static inline void gui_text(int x, int y, const char* text, uint32_t color){
    gui_text_req_t r; r.x = x; r.y = y; r.color = color; r.text = text;
    __asm__ volatile(
        "pushl %%ebx\n\t"
        "movl $12, %%eax\n\t"
        "movl %0, %%ebx\n\t"
        "int $0x80\n\t"
        "popl %%ebx"
        :: "r"((uint32_t)&r) : "eax", "ecx", "edx", "memory");
}
static inline void gui_flush(void){
    __asm__ volatile(
        "pushl %%ebx\n\t"
        "movl $14, %%eax\n\t"
        "int $0x80\n\t"
        "popl %%ebx"
        :: : "eax", "ecx", "edx", "memory");
}
static inline int gui_wait_event(gui_event_t* ev){
    __asm__ volatile(
        "pushl %%ebx\n\t"
        "movl $15, %%eax\n\t"
        "movl %0, %%ebx\n\t"
        "int $0x80\n\t"
        "popl %%ebx"
        :: "r"((uint32_t)ev) : "eax", "ecx", "edx", "memory");
    return ev->type;
}
static inline void gui_destroy(void){
    __asm__ volatile(
        "pushl %%ebx\n\t"
        "movl $11, %%eax\n\t"
        "int $0x80\n\t"
        "popl %%ebx"
        :: : "eax", "ecx", "edx", "memory");
}
#endif
