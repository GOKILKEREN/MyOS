#include <stdint.h>

/* Layout regs_t harus match urutan pusha + int_no/err_code */
typedef struct {
    uint32_t edi, esi, ebp, esp_dummy;
    uint32_t ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags;
} regs_t;

extern void isr128(void);
extern void idt_set_user(int n, uint32_t handler);
extern void kernel_redraw_all(void);
extern void task_exit(void);

typedef struct { int type; int x, y; int btn; int key; } gui_event_t;
typedef struct { int x, y, w, h; char title[32]; } gui_win_req_t;
typedef struct { int x, y, w, h; uint32_t color; } gui_rect_req_t;
typedef struct { int x, y; uint32_t color; const char* text; } gui_text_req_t;

extern void kernel_create_user_window(gui_win_req_t* req);
extern void kernel_destroy_user_window(void);
extern void kernel_user_clear(uint32_t color);
extern void kernel_user_rect(gui_rect_req_t* r);
extern void kernel_user_text(gui_text_req_t* r);
extern void kernel_user_flush(void);
extern void kernel_user_wait_event(gui_event_t* out);

static void (*hook_print)(const char*) = 0;
static int  (*hook_getch)(void) = 0;

void syscall_set_hooks(void (*p)(const char*), int (*g)(void)){
    hook_print = p;
    hook_getch = g;
}

#define SYS_EXIT   0
#define SYS_PRINT  1
#define SYS_GETCH  2
#define SYS_YIELD  3

#define SYS_CREATE_WINDOW   10
#define SYS_DESTROY_WINDOW  11
#define SYS_DRAW_TEXT       12
#define SYS_DRAW_RECT       13
#define SYS_FLUSH           14
#define SYS_WAIT_EVENT      15
#define SYS_CLEAR_WINDOW    16

static int dispatch(uint32_t nr, uint32_t a1, uint32_t a2, uint32_t a3){
    (void)a2; (void)a3;
    switch(nr){
        case SYS_EXIT:
            kernel_destroy_user_window();
            if(hook_print) hook_print("[kernel] user program exited\n");
            kernel_redraw_all();
            task_exit();
            for(;;) __asm__ volatile("cli;hlt");
            return 0;
        case SYS_CREATE_WINDOW:
            kernel_create_user_window((gui_win_req_t*)a1);
            return 0;
        case SYS_DESTROY_WINDOW:
            kernel_destroy_user_window();
            return 0;
        case SYS_DRAW_TEXT:
            kernel_user_text((gui_text_req_t*)a1);
            return 0;
        case SYS_DRAW_RECT:
            kernel_user_rect((gui_rect_req_t*)a1);
            return 0;
        case SYS_FLUSH:
            kernel_user_flush();
            return 0;
        case SYS_WAIT_EVENT:
            kernel_user_wait_event((gui_event_t*)a1);
            return 0;
        case SYS_CLEAR_WINDOW:
            kernel_user_clear(a1);
            return 0;
        case SYS_PRINT:
            if(hook_print) hook_print((const char*)a1);
            return 0;
        case SYS_GETCH:
            if(hook_getch) return hook_getch();
            return -1;
        case SYS_YIELD:
            __asm__ volatile("hlt");
            return 0;
    }
    return -1;
}

void syscall_handler(regs_t* r){
    uint32_t nr = r->eax;
    uint32_t a1 = r->ebx;
    uint32_t a2 = r->ecx;
    uint32_t a3 = r->edx;
    r->eax = (uint32_t)dispatch(nr, a1, a2, a3);
}

void syscall_init(void){
    idt_set_user(0x80, (uint32_t)isr128);
}
