#include <stdint.h>

extern void idt_init(void);
extern void enter_user_mode(void);
extern void scheduler_init(void);
extern uint32_t scheduler_tick(uint32_t esp);
extern int elf_exec(const uint8_t* elf, uint32_t size);
#include "user_programs.h"
void kernel_redraw_all(void);
static void debug_dump_gdt(void);
extern void syscall_init(void);
extern void syscall_set_hooks(void (*p)(const char*), int (*g)(void));
extern void gdt_init(void);
extern void paging_init(uint32_t fb_addr);
extern void msc_set_output(void (*fn)(const char*));
extern int msc_run(const char* src);
extern const uint8_t font8x8[95][8];

static inline void outb(uint16_t p, uint8_t v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline uint8_t inb(uint16_t p){ uint8_t v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }

/* ===== user window + event queue ===== */
#define EV_NONE  0
#define EV_MOUSE 1
#define EV_KEY   2
#define EV_CLOSE 3

typedef struct { int type; int x, y; int btn; int key; } gui_event_t;
typedef struct { int x, y, w, h; char title[32]; } gui_win_req_t;
typedef struct { int x, y, w, h; uint32_t color; } gui_rect_req_t;
typedef struct { int x, y; uint32_t color; const char* text; } gui_text_req_t;

static volatile int user_win_active = 0;
static int user_win_cx = 0, user_win_cy = 0;
static int user_win_cw = 0, user_win_ch = 0;
static int user_win_close_x = 0, user_win_close_y = 0;

static gui_event_t ev_queue[32];
static volatile int ev_head = 0, ev_tail = 0;
static int irq_last_left = 0;

static void ev_push(int type, int x, int y, int btn, int key){
    int nx = (ev_head + 1) % 32;
    if(nx == ev_tail) return;
    ev_queue[ev_head].type = type;
    ev_queue[ev_head].x = x;
    ev_queue[ev_head].y = y;
    ev_queue[ev_head].btn = btn;
    ev_queue[ev_head].key = key;
    ev_head = nx;
}

static uint8_t*  fb = 0;
static uint32_t  fb_pitch = 0, fb_w = 0, fb_h = 0, fb_bpp = 0;

/* ===== composition + cache buffers ===== */
static uint32_t  backbuf[1024*768];
static uint32_t  wallpaper_buf[1024*768];
static uint32_t  user_winbuf[800*600];
static int user_winbuf_w = 0, user_winbuf_h = 0;
static uint32_t  drag_bg[1024*768];
static uint32_t  drag_winbuf[1024*768];

/* ===== render target redirection ===== */
static uint32_t* target_buf = 0;
static int target_w = 0, target_h = 0, target_ox = 0, target_oy = 0;

static void set_target(uint32_t* b, int w, int h, int ox, int oy){
    target_buf = b; target_w = w; target_h = h; target_ox = ox; target_oy = oy;
}
static void clear_target(void){ target_buf = 0; }

/* ===== pixel ops ===== */
static void putpixel(int x, int y, uint32_t c){
    uint32_t px = 0xFF000000u | (c & 0x00FFFFFFu);
    if(target_buf){
        int rx = x - target_ox, ry = y - target_oy;
        if((unsigned)rx < (unsigned)target_w && (unsigned)ry < (unsigned)target_h)
            target_buf[ry*target_w + rx] = px;
        return;
    }
    if((unsigned)x < fb_w && (unsigned)y < fb_h)
        backbuf[y*fb_w + x] = px;
}
static void fill_rect(int x, int y, int w, int h, uint32_t c){
    uint32_t px = 0xFF000000u | (c & 0x00FFFFFFu);
    if(target_buf){
        int x0 = x - target_ox, y0 = y - target_oy;
        if(x0 < 0){ w += x0; x0 = 0; }
        if(y0 < 0){ h += y0; y0 = 0; }
        if(x0 + w > target_w) w = target_w - x0;
        if(y0 + h > target_h) h = target_h - y0;
        if(w <= 0 || h <= 0) return;
        for(int j = 0; j < h; j++){
            uint32_t* row = &target_buf[(y0+j)*target_w + x0];
            int n = w;
            __asm__ volatile("rep stosl" : "+D"(row), "+c"(n) : "a"(px) : "memory");
        }
        return;
    }
    if(x < 0){ w += x; x = 0; }
    if(y < 0){ h += y; y = 0; }
    if(x + w > (int)fb_w) w = (int)fb_w - x;
    if(y + h > (int)fb_h) h = (int)fb_h - y;
    if(w <= 0 || h <= 0) return;
    for(int j = 0; j < h; j++){
        uint32_t* row = &backbuf[(y+j)*fb_w + x];
        int n = w;
        __asm__ volatile("rep stosl" : "+D"(row), "+c"(n) : "a"(px) : "memory");
    }
}
static void hline(int x0, int x1, int y, uint32_t c){
    if(x1 < x0){ int t = x0; x0 = x1; x1 = t; }
    fill_rect(x0, y, x1-x0+1, 1, c);
}
static void blit_full(void){
    if(fb_bpp != 32) return;
    for(uint32_t y = 0; y < fb_h; y++){
        uint32_t* d = (uint32_t*)(fb + y*fb_pitch);
        uint32_t* s = &backbuf[y*fb_w];
        uint32_t n = fb_w;
        __asm__ volatile("rep movsl" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    }
}

/* ===== font ===== */
static void draw_char(int x, int y, char ch, uint32_t fg){
    if(ch < 32 || ch > 126) return;
    const uint8_t* g = font8x8[(int)ch - 32];
    for(int r = 0; r < 8; r++){
        uint8_t bits = g[r];
        if(!bits) continue;
        for(int c = 0; c < 8; c++)
            if(bits & (0x80 >> c)) putpixel(x+c, y+r, fg);
    }
}
static void draw_text(int x, int y, const char* s, uint32_t fg){
    while(*s){ draw_char(x, y, *s++, fg); x += 8; }
}
static void draw_int(int x, int y, int v, uint32_t fg){
    char b[12]; int i = 0;
    if(v == 0) b[i++] = '0';
    else {
        if(v < 0){ b[i++] = '-'; v = -v; }
        char t[12]; int j = 0;
        while(v){ t[j++] = '0' + (v % 10); v /= 10; }
        while(j) b[i++] = t[--j];
    }
    b[i] = 0;
    draw_text(x, y, b, fg);
}

/* ===== string utils ===== */
static int slen(const char* s){ int n = 0; while(s[n]) n++; return n; }
static int seq(const char* a, const char* b){ while(*a && *a == *b){ a++; b++; } return *a == *b; }
static int spre(const char* s, const char* p){ while(*p && *s == *p){ s++; p++; } return *p == 0; }
static int satoi(const char* s){ int n = 0; while(*s >= '0' && *s <= '9'){ n = n*10 + (*s - '0'); s++; } return n; }
static void scpy(char* d, const char* s, int max){ int i = 0; for(; i < max-1 && s[i]; i++) d[i] = s[i]; d[i] = 0; }

/* ===== mouse ===== */
static volatile int mouse_x = 400, mouse_y = 300;
static volatile int mouse_btn = 0;
static volatile int mouse_dirty = 0;
static int mouse_cycle = 0;
static uint8_t mouse_pkt[3];

static void mouse_wait_w(void){ for(int i = 0; i < 100000; i++) if((inb(0x64) & 2) == 0) return; }
static void mouse_wait_r(void){ for(int i = 0; i < 100000; i++) if(inb(0x64) & 1) return; }
static void mouse_write(uint8_t v){
    mouse_wait_w(); outb(0x64, 0xD4);
    mouse_wait_w(); outb(0x60, v);
    mouse_wait_r(); inb(0x60);
}
static void mouse_init(void){
    mouse_wait_w(); outb(0x64, 0xA8);
    mouse_wait_w(); outb(0x64, 0x20);
    mouse_wait_r(); uint8_t st = inb(0x60);
    st |= 2; st &= ~0x20;
    mouse_wait_w(); outb(0x64, 0x60);
    mouse_wait_w(); outb(0x60, st);
    mouse_write(0xF6);
    mouse_write(0xF4);
}
static void mouse_handle_byte(uint8_t d){
    switch(mouse_cycle){
        case 0: if(!(d & 0x08)) return; mouse_pkt[0] = d; mouse_cycle = 1; break;
        case 1: mouse_pkt[1] = d; mouse_cycle = 2; break;
        case 2: {
            mouse_pkt[2] = d; mouse_cycle = 0;
            int dx = (int)mouse_pkt[1];
            int dy = (int)mouse_pkt[2];
            if(mouse_pkt[0] & 0x10) dx -= 256;
            if(mouse_pkt[0] & 0x20) dy -= 256;
            mouse_x += dx; mouse_y -= dy;
            if(mouse_x < 0) mouse_x = 0;
            if(mouse_y < 0) mouse_y = 0;
            if(mouse_x >= (int)fb_w) mouse_x = (int)fb_w - 1;
            if(mouse_y >= (int)fb_h) mouse_y = (int)fb_h - 1;
            mouse_btn = mouse_pkt[0] & 0x07;
            mouse_dirty = 1;

            int nl = mouse_btn & 1;
            if(user_win_active){
                int in_content = (mouse_x >= user_win_cx && mouse_x < user_win_cx + user_win_cw &&
                                  mouse_y >= user_win_cy && mouse_y < user_win_cy + user_win_ch);
                if(in_content){
                    int relx = mouse_x - user_win_cx;
                    int rely = mouse_y - user_win_cy;
                    if(nl && !irq_last_left) ev_push(EV_MOUSE, relx, rely, 1, 0);
                    else if(!nl && irq_last_left) ev_push(EV_MOUSE, relx, rely, 0, 0);
                }
            }
            irq_last_left = nl;
        } break;
    }
}

/* ===== keyboard ===== */
#define KBUF 256
static volatile int kbuf[KBUF];
static volatile int kh = 0, kt = 0;
static void kbd_push(int c){ int nx = (kh+1) % KBUF; if(nx != kt){ kbuf[kh] = c; kh = nx; } }
int  kbd_pop(void){ if(kh == kt) return -1; int c = kbuf[kt]; kt = (kt+1) % KBUF; return c; }

static int shift_count  = 0;
static int ctrl_down    = 0;
static int alt_down     = 0;
static int caps_lock    = 0;
static int num_lock     = 1;
static int kbd_extended = 0;
static volatile uint32_t ticks = 0;
static uint32_t tick_hz = 100;

#define K_UP     0x100
#define K_DOWN   0x101
#define K_LEFT   0x102
#define K_RIGHT  0x103
#define K_HOME   0x104
#define K_END    0x105
#define K_DELETE 0x106
#define K_INSERT 0x107
#define K_PGUP   0x108
#define K_PGDN   0x109

static const char kbd_map[128] = {
    0, 27,'1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,'\\','z','x','c','v','b','n','m',',','.','/',0,
    '*',0,' ',0,0,0,0,0,0,0,0,0,0,0,0,0,
    '7','8','9','-','4','5','6','+','1','2','3','0','.',0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0
};

static const char kbd_shift[128] = {
    0, 27,'!','@','#','$','%','^','&','*','(',')','_','+','\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,'A','S','D','F','G','H','J','K','L',':','"','~',
    0,'|','Z','X','C','V','B','N','M','<','>','?',0,
    '*',0,' ',0,0,0,0,0,0,0,0,0,0,0,0,0,
    '7','8','9','-','4','5','6','+','1','2','3','0','.',0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0
};

static void kbd_irq(void){
    uint8_t sc = inb(0x60);

    if(sc == 0xE0){ kbd_extended = 1; return; }
    if(sc == 0xE1){ kbd_extended = 2; return; }
    if(kbd_extended == 1){
        kbd_extended = 0;
        if(sc & 0x80) return;
        int k = -1;
        switch(sc){
            case 0x48: k = K_UP;     break;
            case 0x50: k = K_DOWN;   break;
            case 0x4B: k = K_LEFT;   break;
            case 0x4D: k = K_RIGHT;  break;
            case 0x47: k = K_HOME;   break;
            case 0x4F: k = K_END;    break;
            case 0x53: k = K_DELETE; break;
            case 0x52: k = K_INSERT; break;
            case 0x49: k = K_PGUP;   break;
            case 0x51: k = K_PGDN;   break;
            case 0x1C: k = '\n';     break;
            case 0x35: k = '/';      break;
        }
        if(k > 0){
            if(user_win_active) ev_push(EV_KEY, 0, 0, 0, k);
            else kbd_push(k);
        }
        return;
    }
    if(kbd_extended == 2){ kbd_extended = 0; return; }

    if(sc & 0x80){
        uint8_t r = sc & 0x7F;
        if(r == 0x2A || r == 0x36){ if(shift_count > 0) shift_count--; }
        else if(r == 0x1D) ctrl_down = 0;
        else if(r == 0x38) alt_down  = 0;
        return;
    }

    uint8_t code = sc & 0x7F;

    if(code == 0x2A || code == 0x36){ shift_count++; return; }
    if(code == 0x1D){ ctrl_down = 1; return; }
    if(code == 0x38){ alt_down  = 1; return; }
    if(code == 0x3A){ caps_lock = !caps_lock; return; }
    if(code == 0x45){ num_lock  = !num_lock;  return; }

    if(!num_lock){
        int k = -1;
        switch(code){
            case 0x48: k = K_UP;     break;
            case 0x50: k = K_DOWN;   break;
            case 0x4B: k = K_LEFT;   break;
            case 0x4D: k = K_RIGHT;  break;
            case 0x47: k = K_HOME;   break;
            case 0x4F: k = K_END;    break;
            case 0x53: k = K_DELETE; break;
            case 0x52: k = K_INSERT; break;
            case 0x49: k = K_PGUP;   break;
            case 0x51: k = K_PGDN;   break;
        }
        if(k > 0){
            if(user_win_active) ev_push(EV_KEY, 0, 0, 0, k);
            else kbd_push(k);
            return;
        }
    }

    char c = (shift_count > 0) ? kbd_shift[code] : kbd_map[code];
    if(caps_lock){
        if(c >= 'a' && c <= 'z') c -= 32;
        else if(c >= 'A' && c <= 'Z') c += 32;
    }
    if(c){
        if(user_win_active) ev_push(EV_KEY, 0, 0, 0, (int)(uint8_t)c);
        else kbd_push((int)(uint8_t)c);
    }
}

uint32_t irq_handler_irq(uint32_t esp){
    uint32_t* regs = (uint32_t*)esp;
    uint32_t n = regs[8];

    if(n == 32) ticks++;
    else if(n == 33) kbd_irq();
    else if(n == 44) mouse_handle_byte(inb(0x60));

    /* EOI dulu sebelum switch */
    if(n >= 40) outb(0xA0, 0x20);
    outb(0x20, 0x20);

    if(n == 32) return scheduler_tick(esp);
    return 0;
}
static void serial_init(void){
    outb(0x3F8+1, 0x00);
    outb(0x3F8+3, 0x80);
    outb(0x3F8+0, 0x03);
    outb(0x3F8+1, 0x00);
    outb(0x3F8+3, 0x03);
    outb(0x3F8+2, 0xC7);
    outb(0x3F8+4, 0x0B);
}
static void serial_putc(char c){
    for(int i = 0; i < 100000; i++) if(inb(0x3FD) & 0x20) break;
    outb(0x3F8, c);
}
static void serial_puts(const char* s){ while(*s) serial_putc(*s++); }
static void serial_hex(uint32_t v){
    const char* hx = "0123456789ABCDEF";
    char b[11]; b[0]='0'; b[1]='x';
    for(int i = 0; i < 8; i++) b[2+i] = hx[(v >> ((7-i)*4)) & 0xF];
    b[10] = 0;
    serial_puts(b);
}
static void debug_dump_gdt(void){
    struct __attribute__((packed)) { uint16_t limit; uint32_t base; } g;
    __asm__ volatile("sgdt %0" : "=m"(g));
    serial_puts("GDTR base=0x"); serial_hex(g.base);
    serial_puts(" limit=0x"); serial_hex(g.limit);
    serial_puts("\n");
    uint8_t* p = (uint8_t*)g.base;
    for(int i = 0; i < 6; i++){
        serial_puts("gdt[");
        serial_putc('0'+i);
        serial_puts("]: ");
        for(int j = 0; j < 8; j++){
            uint8_t b = p[i*8+j];
            serial_putc("0123456789ABCDEF"[b>>4]);
            serial_putc("0123456789ABCDEF"[b&0xF]);
            serial_putc(' ');
        }
        serial_puts("\n");
    }
}

void exc_handler(uint32_t a, uint32_t b){
    serial_puts("[EXC] int="); serial_hex(a);
    serial_puts(" err="); serial_hex(b);
    serial_puts("\n");
    (void)a; (void)b;
    if(fb){ fill_rect(0,0,fb_w,fb_h,0x00A00000); blit_full(); }
    for(;;) __asm__ volatile("cli;hlt");
}

/* ===== filesystem ===== */
#define FS_MAX     24
#define FS_NAMELEN 16
#define FS_DATALEN 16384

typedef struct {
    int used;
    char name[FS_NAMELEN];
    char data[FS_DATALEN];
    int size;
} fs_entry_t;

static fs_entry_t fs[FS_MAX];
static int fs_count = 0;

static int fs_find(const char* name){
    for(int i = 0; i < FS_MAX; i++){
        if(!fs[i].used) continue;
        int m = 1;
        for(int j = 0; j < FS_NAMELEN; j++){
            if(fs[i].name[j] != name[j]){ m = 0; break; }
            if(name[j] == 0) break;
        }
        if(m) return i;
    }
    return -1;
}
static int fs_create(const char* name){
    int idx = fs_find(name);
    if(idx >= 0) return idx;
    for(int i = 0; i < FS_MAX; i++){
        if(fs[i].used) continue;
        fs[i].used = 1;
        scpy(fs[i].name, name, FS_NAMELEN);
        fs[i].size = 0;
        fs[i].data[0] = 0;
        fs_count++;
        return i;
    }
    return -1;
}
static void fs_write(int idx, const char* data, int size){
    if(idx < 0 || idx >= FS_MAX || !fs[idx].used) return;
    if(size > FS_DATALEN-1) size = FS_DATALEN-1;
    for(int i = 0; i < size; i++) fs[idx].data[i] = data[i];
    fs[idx].data[size] = 0;
    fs[idx].size = size;
}
static void fs_delete(int idx){
    if(idx < 0 || idx >= FS_MAX || !fs[idx].used) return;
    fs[idx].used = 0;
    fs_count--;
}
static void fs_init(void){
    for(int i = 0; i < FS_MAX; i++) fs[i].used = 0;
    int i;
    i = fs_create("README.TXT");
    fs_write(i, "Selamat datang di MyOS v1.6!\n\nFPS boost + smooth drag.", 51);
    i = fs_create("NOTES.TXT");
    fs_write(i, "TODO:\n- Heap\n- Paging\n- ATA driver", 31);
    /* auto-register semua user program yang di-embed */
    for(unsigned int k = 0; k < user_program_count; k++){
        const user_prog_t* p = &user_programs[k];
        int ix = fs_create(p->name);
        if(ix < 0) continue;
        int n = (int)p->size;
        if(n > FS_DATALEN-1) n = FS_DATALEN-1;
        for(int j = 0; j < n; j++) fs[ix].data[j] = (char)p->data[j];
        fs[ix].data[n] = 0;
        fs[ix].size = n;
    }
    i = fs_create("HELLO.TXT");
    fs_write(i, "Hello, World!\n", 14);
    i = fs_create("TODO.TXT");
    fs_write(i, "1. Smooth drag\n2. Filesystem\n3. Notepad", 38);
    i = fs_create("DEMO.TXT");
    {
        const char* demo =
            "let x = 5\n"
            "let y = 7\n"
            "print x + y\n"
            "print x * y\n"
            "print \"Halo dari MyScript!\"\n"
            "let i = 0\n"
            "while i < 5 do\n"
            "  print i\n"
            "  let i = i + 1\n"
            "end\n"
            "if x < y then\n"
            "  print \"x lebih kecil dari y\"\n"
            "else\n"
            "  print \"x lebih besar\"\n"
            "end\n";
        int n = 0; while(demo[n]) n++;
        fs_write(i, demo, n);
    }
    i = fs_create("FOR.TXT");
    {
        const char* s =
            "for i in 1..5 do\n"
            "  print i\n"
            "end\n"
            "for k in 1..3 do\n"
            "  print \"k=\" + k\n"
            "end\n";
        int n = 0; while(s[n]) n++;
        fs_write(i, s, n);
    }
    i = fs_create("BUILTIN.TXT");
    {
        const char* s =
            "print abs(0 - 42)\n"
            "print min(3, 7)\n"
            "print max(3, 7)\n"
            "print len(\"hello world\")\n"
            "print str(123) + \"!\"\n"
            "print int(\"456\") + 1\n";
        int n = 0; while(s[n]) n++;
        fs_write(i, s, n);
    }
    i = fs_create("FACT.TXT");
    {
        const char* fact =
            "let n = 5\n"
            "let f = 1\n"
            "while n > 0 do\n"
            "  let f = f * n\n"
            "  let n = n - 1\n"
            "end\n"
            "print \"5! = \" + f\n";
        int n = 0; while(fact[n]) n++;
        fs_write(i, fact, n);
    }
}

/* ===== windows ===== */
#define WIN_COUNT   6
#define TITLE_H     27
#define BTN_W       16
#define BTN_H       14
#define NP_MENUBAR_H 22

enum { T_INFO=0, T_TERM=1, T_NOTEPAD=2, T_CALC=3, T_FILES=4, T_USER=5 };

typedef struct {
    int x, y, w, h;
    int rx, ry, rw, rh;
    int state;
    int type;
    const char* title;
} win_t;

static win_t wins[WIN_COUNT];
static int   zorder[WIN_COUNT];
static int   focused = 1;

static void bring_to_front(int i){
    int pos = -1;
    for(int k = 0; k < WIN_COUNT; k++) if(zorder[k] == i){ pos = k; break; }
    if(pos < 0 || pos == WIN_COUNT-1) return;
    for(int k = pos; k < WIN_COUNT-1; k++) zorder[k] = zorder[k+1];
    zorder[WIN_COUNT-1] = i;
}
static int btn_x(win_t* w, int which){ return w->x + w->w - 22 - which*20; }
static int btn_y(win_t* w){ return w->y + 6; }
static int pt_in(int x, int y, int rx, int ry, int rw, int rh){
    return x >= rx && x < rx+rw && y >= ry && y < ry+rh;
}

/* ===== terminal ===== */
#define TERM_PAD 8
#define TERM_COLS 64
#define TERM_ROWS 47
static char term_buf[TERM_ROWS][TERM_COLS];
static int term_row = 0, term_col = 0;
static char cmd_line[TERM_COLS];
static int  cmd_len = 0;
static int  wallpaper = 0;

static void term_clear(void){
    for(int r = 0; r < TERM_ROWS; r++) for(int c = 0; c < TERM_COLS; c++) term_buf[r][c] = ' ';
    term_row = 0; term_col = 0;
}
static void term_scroll(void){
    for(int r = 0; r < TERM_ROWS-1; r++) for(int c = 0; c < TERM_COLS; c++) term_buf[r][c] = term_buf[r+1][c];
    for(int c = 0; c < TERM_COLS; c++) term_buf[TERM_ROWS-1][c] = ' ';
    term_row = TERM_ROWS-1;
}
static void term_putc(char ch){
    if(ch == '\n'){ term_col = 0; term_row++; }
    else if(ch == '\r'){ term_col = 0; }
    else if(ch == '\b'){ if(term_col > 0){ term_col--; term_buf[term_row][term_col] = ' '; } }
    else {
        term_buf[term_row][term_col] = ch;
        if(++term_col >= TERM_COLS){ term_col = 0; term_row++; }
    }
    if(term_row >= TERM_ROWS) term_scroll();
}
void term_puts(const char* s){ while(*s) term_putc(*s++); }
static void term_putint(int v){
    char b[12]; int i = 0;
    if(v == 0) b[i++] = '0';
    else { if(v < 0){ b[i++] = '-'; v = -v; }
        char t[12]; int j = 0;
        while(v){ t[j++] = '0' + (v % 10); v /= 10; }
        while(j) b[i++] = t[--j]; }
    b[i] = 0; term_puts(b);
}
static void open_win(int i){ wins[i].state = 0; focused = i; bring_to_front(i); }

static void msc_out(const char* s){ term_puts(s); }

static void term_run(const char* line){
    while(*line == ' ') line++;
    if(!*line) return;
    if(seq(line, "help")){
        term_puts("help/clear/about/reboot/halt\n");
        term_puts("uptime/time/ticks\n");
        term_puts("echo TEXT / calc A op B\n");
        term_puts("ls / cat FILE / rm FILE\n");
        term_puts("wallpaper / open APP\n");
    }
    else if(seq(line, "clear")) term_clear();
    else if(seq(line, "about")) term_puts("MyOS v1.6 - smooth drag\n");
    else if(seq(line, "uptime")){ term_puts("up "); term_putint((int)(ticks/tick_hz)); term_puts("s\n"); }
    else if(seq(line, "ticks")){ term_putint((int)ticks); term_putc('\n'); }
    else if(seq(line, "time")){
        int s = (int)(ticks/tick_hz);
        term_putint(12 + s/3600); term_putc(':');
        term_putint((s/60)%60); term_putc(':'); term_putint(s%60); term_putc('\n');
    }
    else if(seq(line, "reboot")){ term_puts("reboot...\n"); while(inb(0x64) & 2); outb(0x64, 0xFE); }
    else if(seq(line, "halt")){ term_puts("halt.\n"); for(;;) __asm__ volatile("hlt"); }
    else if(seq(line, "ls")){
        for(int i = 0; i < FS_MAX; i++){ if(!fs[i].used) continue; term_puts(fs[i].name); term_puts("  "); }
        term_putc('\n');
    }
    else if(seq(line, "pwd")) term_puts("/home/putera\n");
    else if(spre(line, "cat ")){
        int idx = fs_find(line + 4);
        if(idx < 0){ term_puts("not found\n"); return; }
        term_puts(fs[idx].data); term_putc('\n');
    }
    else if(spre(line, "rm ")){
        int idx = fs_find(line + 3);
        if(idx < 0){ term_puts("not found\n"); return; }
        fs_delete(idx);
    }
    else if(spre(line, "open ")){
        const char* p = line + 5;
        if(seq(p, "term")) open_win(1);
        else if(seq(p, "info")) open_win(0);
        else if(seq(p, "notepad")) open_win(2);
        else if(seq(p, "calc")) open_win(3);
        else if(seq(p, "files")) open_win(4);
    }
    else if(spre(line, "exec ")){
        const char* fn = line + 5;
        while(*fn == ' ') fn++;
        term_puts("[exec] loading ");
        term_puts(fn);
        term_puts("...\n");
        int idx = fs_find(fn);
        if(idx < 0){ term_puts("exec: not found\n"); return; }
        kernel_redraw_all();
        int rc = elf_exec((const uint8_t*)fs[idx].data, (uint32_t)fs[idx].size);
        term_puts("[exec] failed rc=");
        if(rc < 0) term_putc('0' + (-rc));
        term_putc('\n');
    }
    else if(spre(line, "run ")){
        const char* fn = line + 4;
        while(*fn == ' ') fn++;
        int idx = fs_find(fn);
        if(idx < 0){ term_puts("run: file not found\n"); return; }
        term_puts("[run] ");
        term_puts(fs[idx].name);
        term_puts("\n");
        msc_run(fs[idx].data);
        term_puts("[done]\n");
    }
    else if(seq(line, "wallpaper")){ wallpaper = (wallpaper + 1) % 4; }
    else if(spre(line, "echo ")){ term_puts(line + 5); term_putc('\n'); }
    else if(spre(line, "calc ")){
        const char* p = line + 5;
        int a = satoi(p); while(*p >= '0' && *p <= '9') p++;
        while(*p == ' ') p++;
        char op = *p++; while(*p == ' ') p++;
        int b = satoi(p);
        int r = 0;
        if(op == '+') r = a+b;
        else if(op == '-') r = a-b;
        else if(op == '*') r = a*b;
        else if(op == '/' && b) r = a/b;
        term_putint(a); term_putc(op); term_putint(b); term_putc('='); term_putint(r); term_putc('\n');
    }
    else { term_puts("? "); term_puts(line); term_putc('\n'); }
}
static void term_input(int c){
    if(c == '\n'){ term_putc('\n'); cmd_line[cmd_len] = 0; term_run(cmd_line); cmd_len = 0; term_puts("> "); }
    else if(c == '\b'){ if(cmd_len > 0){ cmd_len--; term_putc('\b'); } }
    else if(c >= 32 && c < 127 && cmd_len < TERM_COLS-1){ cmd_line[cmd_len++] = (char)c; term_putc((char)c); }
}

/* ===== notepad ===== */
#define NP_MAX 4000
static char np_buf[NP_MAX];
static int  np_len = 0;
static char np_filename[FS_NAMELEN] = "UNTITLED.TXT";
static int  np_menu_open = -1;
static int  np_dialog = 0;
static char np_dlg_buf[FS_NAMELEN];
static int  np_dlg_len = 0;
static char np_status[40] = "untitled";
static int  np_status_ticks = 0;

static void np_set_status(const char* s){ scpy(np_status, s, 40); np_status_ticks = 200; }

static void np_menu_rects(win_t* w, int rects[3][4]){
    int my = w->y + TITLE_H, mx = w->x + 8;
    const char* names[3] = { "File", "Edit", "View" };
    for(int i = 0; i < 3; i++){
        int mw = slen(names[i])*8 + 16;
        rects[i][0] = mx; rects[i][1] = my+2; rects[i][2] = mw; rects[i][3] = NP_MENUBAR_H-4;
        mx += mw + 2;
    }
}
static const char* np_menu_items[3][6] = {
    { "New", "Save", "Save As...", "Open...", "-", "Exit" },
    { "Clear All" },
    { "Line count" },
};
static const int np_menu_counts[3] = { 6, 1, 1 };

static void np_handle_menu(int mi, int ii){
    if(mi == 0){
        if(ii == 0){ np_len = 0; scpy(np_filename, "UNTITLED.TXT", FS_NAMELEN); np_set_status("new"); }
        else if(ii == 1){
            int idx = fs_find(np_filename);
            if(idx < 0) idx = fs_create(np_filename);
            fs_write(idx, np_buf, np_len);
            np_set_status("saved");
        }
        else if(ii == 2){
            np_dialog = 1; np_dlg_len = 0;
            for(int i = 0; i < FS_NAMELEN-1 && np_filename[i]; i++){ np_dlg_buf[i] = np_filename[i]; np_dlg_len = i+1; }
            np_dlg_buf[np_dlg_len] = 0;
        }
        else if(ii == 3){ np_dialog = 2; np_dlg_len = 0; np_dlg_buf[0] = 0; }
        else if(ii == 5){ wins[2].state = 1; }
    } else if(mi == 1){
        if(ii == 0){ np_len = 0; np_set_status("cleared"); }
    }
}
static void np_input(int c){
    if(np_dialog){
        if(c == '\n'){
            np_dlg_buf[np_dlg_len] = 0;
            if(np_dlg_len > 0){
                if(np_dialog == 1){
                    scpy(np_filename, np_dlg_buf, FS_NAMELEN);
                    int idx = fs_find(np_filename);
                    if(idx < 0) idx = fs_create(np_filename);
                    fs_write(idx, np_buf, np_len);
                    np_set_status("saved");
                } else {
                    int idx = fs_find(np_dlg_buf);
                    if(idx >= 0){
                        np_len = fs[idx].size;
                        if(np_len > NP_MAX-1) np_len = NP_MAX-1;
                        for(int i = 0; i < np_len; i++) np_buf[i] = fs[idx].data[i];
                        scpy(np_filename, fs[idx].name, FS_NAMELEN);
                        np_set_status("loaded");
                    } else np_set_status("not found");
                }
            }
            np_dialog = 0;
        } else if(c == 27){ np_dialog = 0; }
        else if(c == '\b'){ if(np_dlg_len > 0) np_dlg_len--; np_dlg_buf[np_dlg_len] = 0; }
        else if(c >= 32 && c < 127 && np_dlg_len < FS_NAMELEN-1){ np_dlg_buf[np_dlg_len++] = (char)c; np_dlg_buf[np_dlg_len] = 0; }
        return;
    }
    if(c == '\b'){ if(np_len > 0) np_len--; }
    else if(c == '\n'){ if(np_len < NP_MAX-1) np_buf[np_len++] = '\n'; }
    else if(c >= 32 && c < 127){ if(np_len < NP_MAX-1) np_buf[np_len++] = (char)c; }
}

/* ===== calculator ===== */
static char calc_disp[16] = "0";
static int  calc_val = 0;
static char calc_op = 0;
static int  calc_entering = 0;
static void calc_set(int v){
    int n = 0; char tmp[12];
    if(v == 0) tmp[n++] = '0';
    else { if(v < 0){ tmp[n++] = '-'; v = -v; }
        char r[12]; int j = 0;
        while(v){ r[j++] = '0' + (v % 10); v /= 10; }
        while(j) tmp[n++] = r[--j]; }
    tmp[n] = 0;
    for(int i = 0; i <= n; i++) calc_disp[i] = tmp[i];
}
static int calc_get(void){
    int v = 0; int i = 0; int neg = 0;
    if(calc_disp[0] == '-'){ neg = 1; i = 1; }
    for(; calc_disp[i]; i++) v = v*10 + (calc_disp[i] - '0');
    return neg ? -v : v;
}
static void calc_apply(void){
    int cur = calc_get(); int r = 0;
    if(calc_op == '+') r = calc_val + cur;
    else if(calc_op == '-') r = calc_val - cur;
    else if(calc_op == '*') r = calc_val * cur;
    else if(calc_op == '/'){ if(cur) r = calc_val / cur; }
    else r = cur;
    calc_set(r);
}
static void calc_push_digit(int d){
    if(!calc_entering){ calc_set(0); calc_entering = 1; }
    int n = slen(calc_disp);
    if(n == 1 && calc_disp[0] == '0'){ calc_disp[0] = '0' + d; calc_disp[1] = 0; }
    else if(n < 10){ calc_disp[n] = '0' + d; calc_disp[n+1] = 0; }
}

static int fm_selected = -1;

/* ===== wallpaper cache ===== */
static int wallpaper_cached = -1;
static void update_wallpaper_cache(void){
    if(wallpaper == wallpaper_cached) return;
    wallpaper_cached = wallpaper;
    for(uint32_t y = 0; y < fb_h; y++){
        uint32_t a = (y*255) / fb_h;
        uint32_t r, g, b;
        if(wallpaper == 0){ r = (0x10*(255-a) + 0x18*a)/255; g = (0x28*(255-a) + 0x48*a)/255; b = (0x50*(255-a) + 0x78*a)/255; }
        else if(wallpaper == 1){ r = (0x48*(255-a) + 0x18*a)/255; g = (0x18*(255-a) + 0x08*a)/255; b = (0x40*(255-a) + 0x28*a)/255; }
        else if(wallpaper == 2){ r = (0x08*(255-a) + 0x30*a)/255; g = (0x38*(255-a) + 0x10*a)/255; b = (0x18*(255-a) + 0x08*a)/255; }
        else { r = (0x28*(255-a) + 0x08*a)/255; g = (0x10*(255-a) + 0x28*a)/255; b = (0x38*(255-a) + 0x68*a)/255; }
        uint32_t c = 0xFF000000u | (r<<16) | (g<<8) | b;
        uint32_t* row = &wallpaper_buf[y*fb_w];
        uint32_t n = fb_w;
        __asm__ volatile("rep stosl" : "+D"(row), "+c"(n) : "a"(c) : "memory");
    }
}
static void draw_wallpaper(void){
    update_wallpaper_cache();
    uint32_t n = fb_w * fb_h;
    uint32_t* d = backbuf;
    uint32_t* s = wallpaper_buf;
    __asm__ volatile("rep movsl" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
}

/* ===== window chrome ===== */
static void draw_window_chrome(win_t* w, int is_focused){
    fill_rect(w->x+8, w->y+8, w->w, w->h, 0x00101820);
    fill_rect(w->x+4, w->y+4, w->w, w->h, 0x00202830);
    fill_rect(w->x, w->y, w->w, w->h, 0x00F0F0F0);
    fill_rect(w->x, w->y, w->w, 1, 0x0080A0C0);
    fill_rect(w->x, w->y+w->h-1, w->w, 1, 0x0080A0C0);
    fill_rect(w->x, w->y, 1, w->h, 0x0080A0C0);
    fill_rect(w->x+w->w-1, w->y, 1, w->h, 0x0080A0C0);
    for(int i = 0; i < w->w-2; i++){
        uint32_t a = (uint32_t)(i*255)/(w->w-2);
        uint32_t r, g, b;
        if(is_focused){ r = (0x18*(255-a) + 0x50*a)/255; g = (0x40*(255-a) + 0x88*a)/255; b = (0x60*(255-a) + 0xB0*a)/255; }
        else          { r = (0x50*(255-a) + 0x80*a)/255; g = (0x58*(255-a) + 0x88*a)/255; b = (0x60*(255-a) + 0x90*a)/255; }
        uint32_t c = (r<<16) | (g<<8) | b;
        for(int j = 1; j < TITLE_H-1; j++) putpixel(w->x+1+i, w->y+j, c);
    }
    hline(w->x+1, w->x+w->w-2, w->y+TITLE_H-1, 0x00183048);
    draw_text(w->x+10, w->y+10, w->title, 0x00FFFFFF);
    fill_rect(btn_x(w,0), btn_y(w), BTN_W, BTN_H, 0x00D05050);
    draw_text(btn_x(w,0)+5, btn_y(w)+3, "x", 0x00FFFFFF);
    fill_rect(btn_x(w,1), btn_y(w), BTN_W, BTN_H, 0x00A0A0A0);
    draw_text(btn_x(w,1)+5, btn_y(w)+3, w->state == 2 ? "-" : "o", 0x00FFFFFF);
    fill_rect(btn_x(w,2), btn_y(w), BTN_W, BTN_H, 0x00A0A0A0);
    draw_text(btn_x(w,2)+5, btn_y(w)+5, "_", 0x00FFFFFF);
}

/* ===== window contents ===== */
static void draw_term_content(void){
    win_t* w = &wins[1];
    int bx = w->x + 1 + TERM_PAD;
    int by = w->y + TITLE_H + TERM_PAD;
    int bwd = w->w - 2 - TERM_PAD*2;
    int bht = w->h - TITLE_H - TERM_PAD*2;
    if(bht < 8 || bwd < 8) return;
    fill_rect(bx, by, bwd, bht, 0x00101018);
    int max_rows = bht / 8; if(max_rows > TERM_ROWS) max_rows = TERM_ROWS;
    int max_cols = bwd / 8; if(max_cols > TERM_COLS) max_cols = TERM_COLS;
    for(int r = 0; r < max_rows; r++){
        for(int c = 0; c < max_cols; c++){
            char ch = term_buf[r][c];
            if(ch != ' ') draw_char(bx + c*8, by + r*8, ch, 0x00E8E8E8);
        }
    }
    if(term_col < max_cols && term_row < max_rows)
        fill_rect(bx + term_col*8, by + term_row*8 + 7, 8, 1, 0x00B0FFB0);
}
static void draw_info_content(void){
    win_t* w = &wins[0];
    int bx = w->x + 12, by = w->y + TITLE_H + 11;
    draw_text(bx, by,      "MyOS v1.6",            0x00101010);
    draw_text(bx, by+22,   "Kernel",               0x00505050);
    draw_text(bx+64, by+22, "monolithic 32-bit",   0x00206080);
    draw_text(bx, by+42,   "Render",               0x00505050);
    draw_text(bx+64, by+42, "cached drag",         0x00206080);
    draw_text(bx, by+62,   "FS files",             0x00505050);
    draw_int(bx+88, by+62, fs_count,               0x00206080);
    draw_text(bx, by+82,   "Uptime",               0x00505050);
    draw_int(bx+64, by+82, (int)(ticks/tick_hz),   0x00206080);
    draw_text(bx+64+8*3+8, by+82, "s",             0x00206080);
    fill_rect(bx, by+114, 8, 8, 0x0020C060);
    draw_text(bx+16, by+114, "All systems OK",     0x00408050);
}
static void draw_notepad_content(void){
    win_t* w = &wins[2];
    int mb_y = w->y + TITLE_H;
    fill_rect(w->x+1, mb_y, w->w-2, NP_MENUBAR_H, 0x00E0E0E0);
    fill_rect(w->x+1, mb_y+NP_MENUBAR_H-1, w->w-2, 1, 0x00A0A0A0);
    int rects[3][4];
    np_menu_rects(w, rects);
    const char* names[3] = { "File", "Edit", "View" };
    for(int i = 0; i < 3; i++){
        int rx = rects[i][0], ry = rects[i][1], rw = rects[i][2], rh = rects[i][3];
        if(np_menu_open == i) fill_rect(rx, ry, rw, rh, 0x00A0C0E0);
        else if(pt_in(mouse_x, mouse_y, rx, ry, rw, rh)) fill_rect(rx, ry, rw, rh, 0x00D0E0F0);
        draw_text(rx+8, ry+5, names[i], 0x00101010);
    }
    int ax = w->x + 6;
    int ay = mb_y + NP_MENUBAR_H + 4;
    int aw = w->w - 12;
    int ah = w->h - TITLE_H - NP_MENUBAR_H - 4 - 20;
    if(aw < 16 || ah < 16) return;
    fill_rect(ax, ay, aw, ah, 0x00FFFFFF);
    fill_rect(ax, ay, aw, 1, 0x00C0C0C0);
    fill_rect(ax, ay+ah-1, aw, 1, 0x00C0C0C0);
    fill_rect(ax, ay, 1, ah, 0x00C0C0C0);
    fill_rect(ax+aw-1, ay, 1, ah, 0x00C0C0C0);
    int px = ax + 4, py = ay + 4;
    int col = 0, row = 0;
    int max_cols = (aw - 8) / 8;
    int max_rows = (ah - 8) / 8;
    for(int i = 0; i < np_len; i++){
        char ch = np_buf[i];
        if(ch == '\n'){ col = 0; row++; if(row >= max_rows) break; continue; }
        if(col >= max_cols){ col = 0; row++; }
        if(row >= max_rows) break;
        draw_char(px + col*8, py + row*8, ch, 0x00101010);
        col++;
    }
    if(row < max_rows) fill_rect(px + col*8, py + row*8 + 7, 8, 1, 0x003A6EA5);
    int sy = w->y + w->h - 18;
    fill_rect(w->x+1, sy, w->w-2, 17, 0x00D0D0D0);
    fill_rect(w->x+1, sy, w->w-2, 1, 0x00A0A0A0);
    draw_text(w->x+8, sy+5, np_filename, 0x00303030);
    if(np_status_ticks > 0){
        draw_text(w->x+8+8*16, sy+5, "|", 0x00606060);
        draw_text(w->x+8+8*18, sy+5, np_status, 0x00308040);
    }
    if(np_menu_open >= 0){
        int mi = np_menu_open;
        int rx = rects[mi][0], ry = rects[mi][1] + rects[mi][3];
        int n = np_menu_counts[mi];
        int item_h = 22;
        int dw = 150;
        for(int i = 0; i < n; i++){
            const char* lbl = np_menu_items[mi][i];
            if(lbl[0] == '-') continue;
            int w2 = slen(lbl)*8 + 30;
            if(w2 > dw) dw = w2;
        }
        int dh = n*item_h + 6;
        fill_rect(rx+4, ry+4, dw, dh, 0x00101820);
        fill_rect(rx, ry, dw, dh, 0x00F8F8F8);
        fill_rect(rx, ry, dw, 1, 0x0080A0C0);
        fill_rect(rx, ry+dh-1, dw, 1, 0x0080A0C0);
        fill_rect(rx, ry, 1, dh, 0x0080A0C0);
        fill_rect(rx+dw-1, ry, 1, dh, 0x0080A0C0);
        for(int i = 0; i < n; i++){
            int iy = ry + 3 + i*item_h;
            const char* lbl = np_menu_items[mi][i];
            if(lbl[0] == '-'){ hline(rx+6, rx+dw-6, iy+item_h/2, 0x00C0C0C0); continue; }
            int hov = pt_in(mouse_x, mouse_y, rx+3, iy, dw-6, item_h);
            if(hov) fill_rect(rx+3, iy, dw-6, item_h, 0x003A6EA5);
            draw_text(rx+12, iy+7, lbl, hov ? 0x00FFFFFF : 0x00101010);
        }
    }
    if(np_dialog){
        int dw = 260, dh = 100;
        int dx = w->x + (w->w - dw)/2;
        int dy = w->y + (w->h - dh)/2;
        fill_rect(dx+4, dy+4, dw, dh, 0x00101820);
        fill_rect(dx, dy, dw, dh, 0x00E8E8E8);
        fill_rect(dx, dy, dw, 1, 0x0080A0C0);
        fill_rect(dx, dy+dh-1, dw, 1, 0x0080A0C0);
        fill_rect(dx, dy, 1, dh, 0x0080A0C0);
        fill_rect(dx+dw-1, dy, 1, dh, 0x0080A0C0);
        draw_text(dx+12, dy+10, np_dialog == 1 ? "Save As (Enter/Esc)" : "Open (Enter/Esc)", 0x00101010);
        int ix = dx+12, iy = dy+40, iw = dw-24, ih = 26;
        fill_rect(ix, iy, iw, ih, 0x00FFFFFF);
        fill_rect(ix, iy, iw, 1, 0x00808080);
        fill_rect(ix, iy+ih-1, iw, 1, 0x00808080);
        fill_rect(ix, iy, 1, ih, 0x00808080);
        fill_rect(ix+iw-1, iy, 1, ih, 0x00808080);
        draw_text(ix+6, iy+9, np_dlg_buf, 0x00101010);
        fill_rect(ix+6+np_dlg_len*8, iy+9+7, 8, 1, 0x003A6EA5);
    }
}
#define CALC_PAD 10
static const char* calc_btns[4][4] = {
    { "7","8","9","/" },
    { "4","5","6","*" },
    { "1","2","3","-" },
    { "C","0","=","+" },
};
static void draw_calc_content(void){
    win_t* w = &wins[3];
    int bx = w->x + CALC_PAD, by = w->y + TITLE_H + CALC_PAD;
    int bwd = w->w - CALC_PAD*2;
    int disp_h = 44;
    fill_rect(bx, by, bwd, disp_h, 0x00101018);
    int n = slen(calc_disp);
    draw_text(bx + bwd - 12 - n*8, by + 14, calc_disp, 0x00B0FFB0);
    int by2 = by + disp_h + 6;
    int bh_area = w->h - TITLE_H - CALC_PAD*3 - disp_h - 6;
    int cw = (bwd - 3*4) / 4;
    int ch = (bh_area - 3*4) / 4;
    for(int r = 0; r < 4; r++) for(int c = 0; c < 4; c++){
        int bxx = bx + c*(cw+4);
        int byy = by2 + r*(ch+4);
        uint32_t col;
        if(c == 3) col = 0x00D08020;
        else if(r == 3 && c == 0) col = 0x00C04040;
        else if(r == 3 && c == 2) col = 0x003A80D0;
        else col = 0x00486078;
        int hov = pt_in(mouse_x, mouse_y, bxx, byy, cw, ch);
        if(hov) col = ((col & 0xFEFEFE) >> 1) + 0x00404040;
        fill_rect(bxx, byy, cw, ch, col);
        fill_rect(bxx, byy, cw, 1, 0x00A0B0C0);
        const char* lbl = calc_btns[r][c];
        int n2 = slen(lbl);
        draw_text(bxx + (cw - n2*8)/2, byy + (ch-8)/2, lbl, 0x00FFFFFF);
    }
}
static void draw_files_content(void){
    win_t* w = &wins[4];
    int cx = w->x + 6, cy = w->y + TITLE_H + 6;
    int cw = w->w - 12;
    int ch = w->h - TITLE_H - 12 - 40;
    if(cw < 40 || ch < 40) return;
    fill_rect(cx, cy, cw, ch, 0x00FFFFFF);
    fill_rect(cx, cy, cw, 1, 0x00A0A0A0);
    fill_rect(cx, cy+ch-1, cw, 1, 0x00A0A0A0);
    fill_rect(cx, cy, 1, ch, 0x00A0A0A0);
    fill_rect(cx+cw-1, cy, 1, ch, 0x00A0A0A0);
    int list_w = cw * 55 / 100;
    fill_rect(cx+list_w, cy+1, 1, ch-2, 0x00C0C0C0);
    int ly = cy + 6;
    int shown = 0;
    for(int i = 0; i < FS_MAX; i++){
        if(!fs[i].used) continue;
        if(ly + 20 > cy + ch - 4) break;
        int hov = pt_in(mouse_x, mouse_y, cx+2, ly-2, list_w-4, 20);
        if(i == fm_selected) fill_rect(cx+2, ly-2, list_w-4, 20, 0x003A6EA5);
        else if(hov)         fill_rect(cx+2, ly-2, list_w-4, 20, 0x00D0E0F0);
        uint32_t fg = (i == fm_selected) ? 0x00FFFFFF : 0x00101010;
        draw_text(cx+8, ly+4, fs[i].name, fg);
        draw_int(cx+list_w-60, ly+4, fs[i].size, fg);
        draw_text(cx+list_w-24, ly+4, "B", fg);
        ly += 20;
        shown++;
    }
    if(!shown) draw_text(cx+8, cy+8, "(no files)", 0x00808080);
    if(fm_selected >= 0 && fm_selected < FS_MAX && fs[fm_selected].used){
        int px = cx + list_w + 8;
        int py = cy + 6;
        int pw = cw - list_w - 14;
        int max_rows = (ch - 12) / 8;
        int max_cols = pw / 8;
        int col = 0, row = 0;
        for(int i = 0; i < fs[fm_selected].size && row < max_rows; i++){
            char ch2 = fs[fm_selected].data[i];
            if(ch2 == '\n'){ col = 0; row++; continue; }
            if(ch2 == '\r') continue;
            if(col >= max_cols){ col = 0; row++; }
            if(row >= max_rows) break;
            draw_char(px + col*8, py + row*8, ch2, 0x00202020);
            col++;
        }
    } else {
        draw_text(cx + list_w + 8, cy + 8, "(select a file)", 0x00909090);
    }
    int by = w->y + w->h - 36;
    const char* btns[3] = { "New", "Delete", "Open" };
    int bw = 90, gap = 8;
    int total = 3*bw + 2*gap;
    int bx = w->x + (w->w - total)/2;
    for(int i = 0; i < 3; i++){
        int bxx = bx + i*(bw+gap);
        uint32_t col = 0x00486078;
        if(i == 1) col = 0x00C04040;
        if(i == 2) col = 0x003A80D0;
        int hov = pt_in(mouse_x, mouse_y, bxx, by, bw, 26);
        if(hov) col = ((col & 0xFEFEFE) >> 1) + 0x00404040;
        fill_rect(bxx, by, bw, 26, col);
        fill_rect(bxx, by, bw, 1, 0x00A0B0C0);
        int nl = slen(btns[i]);
        draw_text(bxx + (bw - nl*8)/2, by+9, btns[i], 0x00FFFFFF);
    }
}

/* ===== desktop icons ===== */
typedef struct { int x, y; const char* label; int win_idx; } icon_t;
static icon_t icons[5] = {
    { 24,  16, "Terminal", 1 },
    { 24,  96, "Info",     0 },
    { 24, 176, "Notepad",  2 },
    { 24, 256, "Calc",     3 },
    { 24, 336, "Files",    4 },
};
static void draw_icon(icon_t* ic){
    int hov = pt_in(mouse_x, mouse_y, ic->x-2, ic->y-2, 68, 76);
    if(hov) fill_rect(ic->x-2, ic->y-2, 68, 76, 0x305080C0);
    uint32_t body = 0x00FFFFFF;
    if(ic->win_idx == 1) body = 0x0020A040;
    else if(ic->win_idx == 0) body = 0x003A6EA5;
    else if(ic->win_idx == 2) body = 0x00D0A040;
    else if(ic->win_idx == 3) body = 0x009060C0;
    else body = 0x00C06040;
    fill_rect(ic->x+8, ic->y+4, 48, 44, 0x00202830);
    fill_rect(ic->x+6, ic->y+2, 48, 44, body);
    fill_rect(ic->x+6, ic->y+2, 48, 1, 0x00FFFFFF);
    draw_text(ic->x+22, ic->y+18, (char[]){ic->label[0],0}, 0x00FFFFFF);
    int n = slen(ic->label);
    draw_text(ic->x + (60 - n*8)/2, ic->y+50, ic->label, 0x00FFFFFF);
}
static void draw_icons(void){ for(int i = 0; i < 5; i++) draw_icon(&icons[i]); }

/* ===== taskbar & menus ===== */
static int start_open = 0;
static int ctx_open = 0;
static int ctx_x = 0, ctx_y = 0;
static int fps_display = 0;

static void draw_taskbar(void){
    int th = 36, ty = fb_h - th;
    fill_rect(0, ty, fb_w, th, 0x00182028);
    fill_rect(0, ty, fb_w, 1, 0x005070A0);
    fill_rect(8, ty+6, 78, th-12, start_open ? 0x004A88C8 : 0x002A4E75);
    fill_rect(8, ty+6, 78, 1, 0x005080C0);
    draw_text(20, ty+12, "MyOS", 0x00FFFFFF);
    int cx = 96;
    const char* names[WIN_COUNT] = { "Info", ">_ Term", "Note", "Calc", "Files" };
    for(int i = 0; i < WIN_COUNT; i++){
        if(wins[i].state == 1) continue;
        int cw = 90;
        uint32_t cc = (i == focused) ? 0x004A88C8 : 0x00304858;
        fill_rect(cx, ty+6, cw, th-12, cc);
        fill_rect(cx, ty+6, cw, 1, 0x005080C0);
        draw_text(cx+10, ty+12, names[i], 0x00FFFFFF);
        cx += cw + 4;
    }
    draw_text(fb_w-160, ty+12, "FPS:", 0x00A0FFA0);
    draw_int(fb_w-120, ty+12, fps_display, 0x00A0FFA0);
    int s = (int)(ticks/tick_hz);
    int h = 12 + s/3600, m = (s/60)%60, sec = s%60;
    char tb[9];
    tb[0] = '0'+(h/10)%10; tb[1] = '0'+h%10; tb[2] = ':';
    tb[3] = '0'+(m/10)%10; tb[4] = '0'+m%10; tb[5] = ':';
    tb[6] = '0'+(sec/10)%10; tb[7] = '0'+sec%10; tb[8] = 0;
    draw_text(fb_w-70, ty+12, tb, 0x00FFFFFF);
}
static const char* start_items[] = { ">_ Terminal", "System Info", "Notepad", "Calculator", "File Manager", "-", "About MyOS" };
#define START_COUNT 7
#define START_W 180
#define START_ITEM_H 28

static void draw_start_menu(void){
    if(!start_open) return;
    int th = 36;
    int menu_h = START_COUNT*START_ITEM_H + 8;
    int mx = 8, my = fb_h - th - menu_h - 4;
    fill_rect(mx+4, my+4, START_W, menu_h, 0x00101820);
    fill_rect(mx, my, START_W, menu_h, 0x00E8ECF2);
    fill_rect(mx, my, START_W, 1, 0x005070A0);
    fill_rect(mx, my+menu_h-1, START_W, 1, 0x005070A0);
    fill_rect(mx, my, 1, menu_h, 0x005070A0);
    fill_rect(mx+START_W-1, my, 1, menu_h, 0x005070A0);
    for(int i = 0; i < START_COUNT; i++){
        int iy = my + 4 + i*START_ITEM_H;
        int hov = pt_in(mouse_x, mouse_y, mx+4, iy, START_W-8, START_ITEM_H);
        if(seq(start_items[i], "-")){ hline(mx+8, mx+START_W-8, iy+START_ITEM_H/2, 0x00C0C8D0); continue; }
        if(hov) fill_rect(mx+4, iy, START_W-8, START_ITEM_H, 0x003A6EA5);
        draw_text(mx+16, iy+10, start_items[i], hov ? 0x00FFFFFF : 0x00203040);
    }
}
static void draw_ctx_menu(void){
    if(!ctx_open) return;
    const char* items[] = { "Refresh", "Change Wallpaper", "Open Terminal", "Open File Manager" };
    int n = 4, w = 210, ih = 26;
    int h = n*ih + 8;
    int mx = ctx_x, my = ctx_y;
    if(mx + w > (int)fb_w) mx = fb_w - w - 4;
    if(my + h > (int)fb_h - 36) my = fb_h - 36 - h - 4;
    fill_rect(mx+4, my+4, w, h, 0x00101820);
    fill_rect(mx, my, w, h, 0x00E8ECF2);
    fill_rect(mx, my, w, 1, 0x005070A0);
    fill_rect(mx, my+h-1, w, 1, 0x005070A0);
    fill_rect(mx, my, 1, h, 0x005070A0);
    fill_rect(mx+w-1, my, 1, h, 0x005070A0);
    for(int i = 0; i < n; i++){
        int iy = my + 4 + i*ih;
        int hov = pt_in(mouse_x, mouse_y, mx+4, iy, w-8, ih);
        if(hov) fill_rect(mx+4, iy, w-8, ih, 0x003A6EA5);
        draw_text(mx+12, iy+9, items[i], hov ? 0x00FFFFFF : 0x00203040);
    }
}

/* ===== desktop compositor ===== */
static void user_winbuf_to_backbuf(void);

static void draw_one_window(int i){
    win_t* w = &wins[i];
    if(w->state == 1) return;
    draw_window_chrome(w, i == focused);
    if(i == 0) draw_info_content();
    else if(i == 1) draw_term_content();
    else if(i == 2) draw_notepad_content();
    else if(i == 3) draw_calc_content();
    else if(i == 4) draw_files_content();
    else if(i == 5) user_winbuf_to_backbuf();
}
static void draw_desktop(void){
    draw_wallpaper();
    draw_icons();
    for(int k = 0; k < WIN_COUNT; k++){
        int i = zorder[k];
        if(wins[i].state == 1) continue;
        draw_one_window(i);
    }
    draw_taskbar();
    draw_start_menu();
    draw_ctx_menu();
}

/* ===== cursor (transition) ===== */
#define CUR_W 12
#define CUR_H 19
static const uint8_t cur_bmp[CUR_H][CUR_W] = {
    {1,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,0,0,0,0,0,0,0,0,0,0},
    {1,2,1,0,0,0,0,0,0,0,0,0},
    {1,2,2,1,0,0,0,0,0,0,0,0},
    {1,2,2,2,1,0,0,0,0,0,0,0},
    {1,2,2,2,2,1,0,0,0,0,0,0},
    {1,2,2,2,2,2,1,0,0,0,0,0},
    {1,2,2,2,2,2,2,1,0,0,0,0},
    {1,2,2,2,2,2,2,2,1,0,0,0},
    {1,2,2,2,2,2,2,2,2,1,0,0},
    {1,2,2,2,2,2,2,2,2,2,1,0},
    {1,2,2,2,2,2,1,1,1,1,1,1},
    {1,2,2,1,2,2,1,0,0,0,0,0},
    {1,2,1,0,1,2,2,1,0,0,0,0},
    {1,1,0,0,1,2,2,1,0,0,0,0},
    {1,0,0,0,0,1,2,2,1,0,0,0},
    {0,0,0,0,0,1,2,2,1,0,0,0},
    {0,0,0,0,0,0,1,2,1,0,0,0},
    {0,0,0,0,0,0,1,1,0,0,0,0},
};
static int cur_sx = -1, cur_sy = -1;

static void cursor_transition(int old_x, int old_y, int new_x, int new_y){
    int x0 = new_x, y0 = new_y;
    int x1 = new_x + CUR_W, y1 = new_y + CUR_H;
    if(old_x >= 0){
        if(old_x < x0) x0 = old_x;
        if(old_y < y0) y0 = old_y;
        if(old_x + CUR_W > x1) x1 = old_x + CUR_W;
        if(old_y + CUR_H > y1) y1 = old_y + CUR_H;
    }
    if(x0 < 0) x0 = 0;
    if(y0 < 0) y0 = 0;
    if(x1 > (int)fb_w) x1 = fb_w;
    if(y1 > (int)fb_h) y1 = fb_h;
    for(int y = y0; y < y1; y++){
        uint32_t* fbp = (uint32_t*)(fb + y*fb_pitch);
        uint32_t* bbp = &backbuf[y*fb_w];
        for(int x = x0; x < x1; x++){
            int nc = x - new_x, nr = y - new_y;
            if(nc >= 0 && nc < CUR_W && nr >= 0 && nr < CUR_H){
                uint8_t v = cur_bmp[nr][nc];
                if(v == 1){ fbp[x] = 0xFF000000u; continue; }
                if(v == 2){ fbp[x] = 0xFFFFFFFFu; continue; }
            }
            if(old_x >= 0){
                int oc = x - old_x, orr = y - old_y;
                if(oc >= 0 && oc < CUR_W && orr >= 0 && orr < CUR_H){
                    fbp[x] = 0xFF000000u | (bbp[x] & 0x00FFFFFFu);
                }
            }
        }
    }
    cur_sx = new_x; cur_sy = new_y;
}

/* ===== interaction handlers ===== */
static void toggle_max(int i){
    win_t* w = &wins[i];
    if(w->state == 2){ w->x = w->rx; w->y = w->ry; w->w = w->rw; w->h = w->rh; w->state = 0; }
    else {
        w->rx = w->x; w->ry = w->y; w->rw = w->w; w->rh = w->h;
        w->x = 4; w->y = 4; w->w = (int)fb_w - 8; w->h = (int)fb_h - 36 - 8; w->state = 2;
    }
}
static void handle_calc_click(int mx, int my){
    win_t* w = &wins[3];
    int bx = w->x + CALC_PAD, by = w->y + TITLE_H + CALC_PAD;
    int bwd = w->w - CALC_PAD*2;
    int disp_h = 44;
    int by2 = by + disp_h + 6;
    int bh_area = w->h - TITLE_H - CALC_PAD*3 - disp_h - 6;
    int cw = (bwd - 3*4) / 4;
    int ch = (bh_area - 3*4) / 4;
    for(int r = 0; r < 4; r++) for(int c = 0; c < 4; c++){
        int bxx = bx + c*(cw+4);
        int byy = by2 + r*(ch+4);
        if(!pt_in(mx, my, bxx, byy, cw, ch)) continue;
        const char* lbl = calc_btns[r][c];
        if(lbl[0] >= '0' && lbl[0] <= '9'){ calc_push_digit(lbl[0] - '0'); return; }
        if(lbl[0] == 'C'){ calc_set(0); calc_val = 0; calc_op = 0; calc_entering = 0; return; }
        if(lbl[0] == '='){ calc_apply(); calc_op = 0; calc_entering = 0; return; }
        calc_apply(); calc_val = calc_get(); calc_op = lbl[0]; calc_entering = 0;
        return;
    }
}
static void handle_files_click(int mx, int my){
    win_t* w = &wins[4];
    int cx = w->x + 6, cy = w->y + TITLE_H + 6;
    int cw = w->w - 12;
    int ch = w->h - TITLE_H - 12 - 40;
    int list_w = cw * 55 / 100;
    if(pt_in(mx, my, cx, cy, list_w, ch)){
        int ly = cy + 6;
        for(int i = 0; i < FS_MAX; i++){
            if(!fs[i].used) continue;
            if(ly + 20 > cy + ch - 4) break;
            if(pt_in(mx, my, cx+2, ly-2, list_w-4, 20)){ fm_selected = i; return; }
            ly += 20;
        }
        return;
    }
    int by = w->y + w->h - 36;
    int bw = 90, gap = 8;
    int total = 3*bw + 2*gap;
    int bx = w->x + (w->w - total)/2;
    for(int i = 0; i < 3; i++){
        int bxx = bx + i*(bw+gap);
        if(!pt_in(mx, my, bxx, by, bw, 26)) continue;
        if(i == 0){
            int idx = fs_create("NEW.TXT");
            if(idx >= 0){ fs_write(idx, "", 0); fm_selected = idx; }
        } else if(i == 1){
            if(fm_selected >= 0){ fs_delete(fm_selected); fm_selected = -1; }
        } else {
            if(fm_selected >= 0 && fs[fm_selected].used){
                np_len = fs[fm_selected].size;
                if(np_len > NP_MAX-1) np_len = NP_MAX-1;
                for(int j = 0; j < np_len; j++) np_buf[j] = fs[fm_selected].data[j];
                scpy(np_filename, fs[fm_selected].name, FS_NAMELEN);
                np_set_status("loaded");
                open_win(2);
            }
        }
        return;
    }
}
static void handle_notepad_click(int mx, int my){
    win_t* w = &wins[2];
    if(np_dialog){
        int dw = 260, dh = 100;
        int dx = w->x + (w->w - dw)/2;
        int dy = w->y + (w->h - dh)/2;
        if(pt_in(mx, my, dx, dy, dw, dh)) return;
        np_dialog = 0; return;
    }
    if(np_menu_open >= 0){
        int rects[3][4];
        np_menu_rects(w, rects);
        int mi = np_menu_open;
        int rx = rects[mi][0], ry = rects[mi][1] + rects[mi][3];
        int n = np_menu_counts[mi];
        int item_h = 22;
        int dw = 150;
        for(int i = 0; i < n; i++){
            const char* lbl = np_menu_items[mi][i];
            if(lbl[0] == '-') continue;
            int w2 = slen(lbl)*8 + 30;
            if(w2 > dw) dw = w2;
        }
        if(pt_in(mx, my, rx, ry, dw, n*item_h + 6)){
            for(int i = 0; i < n; i++){
                int iy = ry + 3 + i*item_h;
                const char* lbl = np_menu_items[mi][i];
                if(lbl[0] == '-') continue;
                if(pt_in(mx, my, rx+3, iy, dw-6, item_h)){ np_handle_menu(mi, i); break; }
            }
            np_menu_open = -1;
            return;
        }
        np_menu_open = -1;
        return;
    }
    int rects[3][4];
    np_menu_rects(w, rects);
    for(int i = 0; i < 3; i++){
        if(pt_in(mx, my, rects[i][0], rects[i][1], rects[i][2], rects[i][3])){
            np_menu_open = i; return;
        }
    }
}

/* ===== drag state ===== */
static int drag_win = -1;
static int drag_off_x = 0, drag_off_y = 0;
static int drag_ox = 0, drag_oy = 0;
static int drag_w = 0, drag_h = 0;

static void begin_drag(int i){
    win_t* w = &wins[i];
    drag_win = i;
    drag_off_x = mouse_x - w->x;
    drag_off_y = mouse_y - w->y;
    drag_ox = w->x; drag_oy = w->y;
    drag_w = w->w; drag_h = w->h;

    /* 1. Snapshot desktop tanpa window ini ke drag_bg */
    int saved_state = w->state;
    w->state = 1;
    draw_desktop();
    {
        uint32_t n = fb_w * fb_h;
        uint32_t* d = drag_bg;
        uint32_t* s = backbuf;
        __asm__ volatile("rep movsl" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    }
    w->state = saved_state;

    /* 2. Render window ke drag_winbuf (dengan shadow, offset -8) */
    set_target(drag_winbuf, drag_w + 8, drag_h + 8, drag_ox, drag_oy);
    draw_one_window(i);
    clear_target();
}

static void drag_step(void){
    win_t* w = &wins[drag_win];
    int ox = w->x, oy = w->y;
    int nx = mouse_x - drag_off_x;
    int ny = mouse_y - drag_off_y;
    if(nx < 0) nx = 0;
    if(ny < 0) ny = 0;
    if(nx + w->w > (int)fb_w) nx = fb_w - w->w;
    if(ny + w->h > (int)fb_h - 36) ny = fb_h - 36 - w->h;
    if(nx == ox && ny == oy) return;
    w->x = nx; w->y = ny;

    int bx0 = (ox < nx) ? ox : nx;
    int by0 = (oy < ny) ? oy : ny;
    int bx1 = ((ox > nx) ? ox : nx) + drag_w + 8;
    int by1 = ((oy > ny) ? oy : ny) + drag_h + 8;
    if(bx0 < 0) bx0 = 0;
    if(by0 < 0) by0 = 0;
    if(bx1 > (int)fb_w) bx1 = fb_w;
    if(by1 > (int)fb_h) by1 = fb_h;
    int bw = bx1 - bx0, bh = by1 - by0;
    if(bw <= 0 || bh <= 0) return;

    /* 1. Restore bbox dari drag_bg ke BACKBUF */
    for(int y = 0; y < bh; y++){
        uint32_t* d = &backbuf[(by0+y)*fb_w + bx0];
        uint32_t* s = &drag_bg[(by0+y)*fb_w + bx0];
        uint32_t n = bw;
        __asm__ volatile("rep movsl" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    }

    /* 2. Copy drag_winbuf ke BACKBUF di posisi baru */
    int dx0 = nx, dy0 = ny;
    int dw = drag_w + 8, dh = drag_h + 8;
    if(dx0 < 0){
        dw += dx0; dx0 = 0;
    }
    if(dy0 < 0){
        dh += dy0; dy0 = 0;
    }
    if(dx0 + dw > (int)fb_w) dw = fb_w - dx0;
    if(dy0 + dh > (int)fb_h) dh = fb_h - dy0;
    if(dw > 0 && dh > 0){
        for(int y = 0; y < dh; y++){
            uint32_t* d = &backbuf[(dy0+y)*fb_w + dx0];
            uint32_t* s = &drag_winbuf[y*(drag_w+8) + (dx0 - nx)];
            uint32_t n = dw;
            __asm__ volatile("rep movsl" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
        }
    }

    /* 3. Blit BACKBUF bbox ke fb */
    for(int y = 0; y < bh; y++){
        uint32_t* d = (uint32_t*)(fb + (by0+y)*fb_pitch + bx0*4);
        uint32_t* s = &backbuf[(by0+y)*fb_w + bx0];
        uint32_t n = bw;
        __asm__ volatile("rep movsl" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    }
}

/* ============================================================
 * MAIN
 * ============================================================ */
void kernel_redraw_all(void){
    draw_desktop();
    blit_full();
}

/* ===== user window kernel wrappers (called from syscall.c) ===== */
void kernel_create_user_window(gui_win_req_t* req){
    win_t* w = &wins[5];
    w->x = req->x; w->y = req->y; w->w = req->w; w->h = req->h;
    w->rx = w->x; w->ry = w->y; w->rw = w->w; w->rh = w->h;
    w->state = 0;
    w->type = 99;
    static char title_buf[32];
    for(int i = 0; i < 32; i++) title_buf[i] = req->title[i];
    w->title = title_buf;

    user_win_cx = w->x + 1;
    user_win_cy = w->y + TITLE_H;
    user_win_cw = w->w - 2;
    user_win_ch = w->h - TITLE_H - 1;
    user_winbuf_w = user_win_cw;
    user_winbuf_h = user_win_ch;
    for(int k = 0; k < user_winbuf_w * user_winbuf_h; k++) user_winbuf[k] = 0xFFFFFFFF;
    user_win_close_x = btn_x(w, 0);
    user_win_close_y = btn_y(w);

    bring_to_front(5);
    focused = 5;
    user_win_active = 1;
    irq_last_left = 0;
    ev_head = 0; ev_tail = 0;

    fill_rect(user_win_cx, user_win_cy, user_win_cw, user_win_ch, 0x00FFFFFF);
    draw_desktop();
    blit_full();
    cur_sx = -1;
    cursor_transition(-1, -1, mouse_x, mouse_y);
}
void kernel_destroy_user_window(void){
    user_win_active = 0;
    wins[5].state = 1;
    ev_head = 0; ev_tail = 0;
    focused = 1;
}
void kernel_user_clear(uint32_t color){
    if(!user_win_active) return;
    set_target(user_winbuf, user_winbuf_w, user_winbuf_h, user_win_cx, user_win_cy);
    fill_rect(user_win_cx, user_win_cy, user_win_cw, user_win_ch, color);
    clear_target();
}
void kernel_user_rect(gui_rect_req_t* r){
    if(!user_win_active) return;
    set_target(user_winbuf, user_winbuf_w, user_winbuf_h, user_win_cx, user_win_cy);
    fill_rect(user_win_cx + r->x, user_win_cy + r->y, r->w, r->h, r->color);
    clear_target();
}
void kernel_user_text(gui_text_req_t* r){
    if(!user_win_active) return;
    set_target(user_winbuf, user_winbuf_w, user_winbuf_h, user_win_cx, user_win_cy);
    draw_text(user_win_cx + r->x, user_win_cy + r->y, r->text, r->color);
    clear_target();
}
static void user_winbuf_to_backbuf(void){
    if(!user_win_active) return;
    if(user_winbuf_w <= 0 || user_winbuf_h <= 0) return;

    if(target_buf){
        int wx = user_win_cx - target_ox;
        int wy = user_win_cy - target_oy;
        for(int y = 0; y < user_winbuf_h; y++){
            int ty = wy + y;
            if(ty < 0 || ty >= target_h) continue;
            uint32_t* d = &target_buf[ty * target_w];
            uint32_t* s = &user_winbuf[y * user_winbuf_w];
            for(int x = 0; x < user_winbuf_w; x++){
                int tx = wx + x;
                if(tx < 0 || tx >= target_w) continue;
                d[tx] = s[x];
            }
        }
        return;
    }

    for(int y = 0; y < user_winbuf_h; y++){
        uint32_t* d = &backbuf[(user_win_cy + y) * fb_w + user_win_cx];
        uint32_t* s = &user_winbuf[y * user_winbuf_w];
        int n = user_winbuf_w;
        __asm__ volatile("rep movsl" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    }
}

void kernel_user_flush(void){
    if(!user_win_active) return;

    user_winbuf_to_backbuf();

    int x0 = user_win_cx;
    int y0 = user_win_cy;
    int w  = user_winbuf_w;
    int h  = user_winbuf_h;
    if(x0 < 0){ w += x0; x0 = 0; }
    if(y0 < 0){ h += y0; y0 = 0; }
    if(x0 + w > (int)fb_w) w = (int)fb_w - x0;
    if(y0 + h > (int)fb_h) h = (int)fb_h - y0;
    if(w <= 0 || h <= 0) return;

    for(int y = 0; y < h; y++){
        uint32_t* d = (uint32_t*)(fb + (y0+y)*fb_pitch + x0*4);
        uint32_t* s = &backbuf[(y0+y)*fb_w + x0];
        uint32_t n = w;
        __asm__ volatile("rep movsl" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    }

    cursor_transition(cur_sx, cur_sy, mouse_x, mouse_y);
}
void kernel_user_wait_event(gui_event_t* out){
    while(ev_head == ev_tail){
        __asm__ volatile("hlt");
    }
    *out = ev_queue[ev_tail];
    ev_tail = (ev_tail + 1) % 32;
}

void kernel_main(uint32_t magic, uint32_t mbi){
    (void)magic;
    uint8_t* p = (uint8_t*)(uintptr_t)mbi;
    fb = (uint8_t*)(uintptr_t)(*(uint32_t*)(p + 88));
    fb_pitch = *(uint32_t*)(p + 96);
    fb_w     = *(uint32_t*)(p + 100);
    fb_h     = *(uint32_t*)(p + 104);
    fb_bpp   = *(uint8_t*)(p + 108);
    if(!fb || fb_w < 100 || fb_h < 100 || fb_w > 1024 || fb_h > 768){
        for(;;) __asm__ volatile("hlt");
    }

    { uint32_t div = 1193182 / 100;
      outb(0x43, 0x36);
      outb(0x40, div & 0xFF);
      outb(0x40, (div >> 8) & 0xFF); }

    /* STAGE 1: aktifkan paging (identity map) */
    serial_init();
    serial_puts("[boot] serial ready\n");

    paging_init((uint32_t)(uintptr_t)fb);

    /* STAGE 2: GDT + TSS */
    gdt_init();

    idt_init();
    scheduler_init();
    mouse_init();
    fs_init();

    wins[0] = (win_t){ 340, 100, 340, 230, 0,0,0,0, 0, T_INFO,    "System Info" };
    wins[1] = (win_t){ 180,  60, 520, 420, 0,0,0,0, 0, T_TERM,    "Terminal" };
    wins[2] = (win_t){ 260, 120, 480, 360, 0,0,0,0, 1, T_NOTEPAD, "Notepad" };
    wins[3] = (win_t){ 420, 160, 260, 340, 0,0,0,0, 1, T_CALC,    "Calculator" };
    wins[4] = (win_t){ 300, 140, 480, 320, 0,0,0,0, 1, T_FILES,   "File Manager" };
    wins[5] = (win_t){ 200, 100, 400, 300, 0,0,0,0, 1, T_USER,    "User Program" };
    for(int i = 0; i < WIN_COUNT; i++) zorder[i] = i;
    focused = 1;

    msc_set_output(msc_out);
    term_clear();
    term_puts("MyOS v1.6 - + scripting engine\n");
    term_puts("Coba: run DEMO.TXT\n\n");
    term_puts("MyOS v1.6 - smooth drag\n");
    term_puts("Ketik 'help'.\n\n> ");

    /* STAGE 3: syscall setup + test */
    syscall_init();
    syscall_set_hooks(term_puts, kbd_pop);

    {
        const char* msg = "[syscall] int 0x80 works!\n";
        __asm__ volatile(
            "mov $1, %%eax\n"
            "mov %0, %%ebx\n"
            "int $0x80\n"
            :: "r"((uint32_t)msg)
            : "eax", "ebx", "memory");
    }

    draw_desktop();
    blit_full();
    cursor_transition(-1, -1, mouse_x, mouse_y);

    uint32_t last_sec = 0xFFFFFFFF;
    int last_left = 0, last_right = 0;

    uint32_t fps_frames = 0;
    uint32_t fps_last_tick = 0;

    for(;;){
        __asm__ volatile("hlt");
        int ui_dirty = 0;

        int c;
        while((c = kbd_pop()) >= 0){
            if(focused == 1) term_input((char)c);
            else if(focused == 2) np_input((char)c);
            ui_dirty = 1;
        }

        uint32_t sec = ticks / tick_hz;
        if(sec != last_sec){ last_sec = sec; ui_dirty = 1; }
        if(np_status_ticks > 0){ np_status_ticks--; if(np_status_ticks == 0) ui_dirty = 1; }

        if(ticks - fps_last_tick >= tick_hz){
            fps_display = fps_frames;
            fps_frames = 0;
            fps_last_tick = ticks;
            ui_dirty = 1;
        }

        int cursor_moved = 0;
        if(mouse_dirty){
            mouse_dirty = 0;
            cursor_moved = 1;

            int left_now  = mouse_btn & 1;
            int right_now = (mouse_btn >> 1) & 1;
            int click_edge = left_now && !last_left;
            int right_edge = right_now && !last_right;
            int release    = !left_now && last_left;

            if(click_edge || right_edge || release){
                if(user_win_active){
                    /* user window aktif: cuma full redraw kalau klik di luar user window */
                    int ux = wins[5].x, uy = wins[5].y, uw = wins[5].w, uh = wins[5].h;
                    if(mouse_x < ux || mouse_x >= ux+uw || mouse_y < uy || mouse_y >= uy+uh)
                        ui_dirty = 1;
                } else {
                    ui_dirty = 1;
                }
            }

            if(click_edge && user_win_active &&
               pt_in(mouse_x, mouse_y, wins[5].x, wins[5].y, wins[5].w, wins[5].h)){
                /* STAGE 12a: user window title bar */
                if(pt_in(mouse_x, mouse_y, wins[5].x, wins[5].y, wins[5].w, TITLE_H)){
                    if(pt_in(mouse_x, mouse_y, btn_x(&wins[5], 0), btn_y(&wins[5]), BTN_W, BTN_H)){
                        ev_push(EV_CLOSE, 0, 0, 0, 0);
                    } else {
                        begin_drag(5);
                    }
                }
                ui_dirty = 1;
            }
            else if(click_edge){
                int handled = 0;
                if(start_open){
                    int th = 36;
                    int menu_h = START_COUNT*START_ITEM_H + 8;
                    int mx = 8, my = fb_h - th - menu_h - 4;
                    if(pt_in(mouse_x, mouse_y, mx, my, START_W, menu_h)){
                        for(int i = 0; i < START_COUNT; i++){
                            int iy = my + 4 + i*START_ITEM_H;
                            if(!pt_in(mouse_x, mouse_y, mx+4, iy, START_W-8, START_ITEM_H)) continue;
                            if(seq(start_items[i], "-")) break;
                            if(i == 0) open_win(1);
                            else if(i == 1) open_win(0);
                            else if(i == 2) open_win(2);
                            else if(i == 3) open_win(3);
                            else if(i == 4) open_win(4);
                            break;
                        }
                        start_open = 0; handled = 1;
                    } else start_open = 0;
                }
                else if(ctx_open){
                    int n = 4, w = 210, ih = 26, h = n*ih + 8;
                    int mx = ctx_x, my = ctx_y;
                    if(mx + w > (int)fb_w) mx = fb_w - w - 4;
                    if(my + h > (int)fb_h - 36) my = fb_h - 36 - h - 4;
                    if(pt_in(mouse_x, mouse_y, mx, my, w, h)){
                        int idx = (mouse_y - my - 4) / ih;
                        if(idx == 1){ wallpaper = (wallpaper+1)%4; }
                        else if(idx == 2){ open_win(1); }
                        else if(idx == 3){ open_win(4); }
                        ctx_open = 0; handled = 1;
                    } else ctx_open = 0;
                }
                if(!handled){
                    for(int i = 0; i < 5; i++){
                        if(pt_in(mouse_x, mouse_y, icons[i].x-2, icons[i].y-2, 68, 76)){
                            open_win(icons[i].win_idx); handled = 1; break;
                        }
                    }
                }
                if(!handled){
                    if(pt_in(mouse_x, mouse_y, 8, fb_h-36+6, 78, 24)){
                        start_open = 1; ctx_open = 0; handled = 1;
                    } else {
                        int cx = 96;
                        for(int i = 0; i < WIN_COUNT; i++){
                            if(wins[i].state == 1) continue;
                            int cw = 90;
                            if(pt_in(mouse_x, mouse_y, cx, fb_h-36+6, cw, 24)){
                                open_win(i); handled = 1; break;
                            }
                            cx += cw + 4;
                        }
                    }
                }
                if(!handled){
                    for(int k = WIN_COUNT-1; k >= 0; k--){
                        int i = zorder[k];
                        win_t* w = &wins[i];
                        if(w->state == 1) continue;
                        if(!pt_in(mouse_x, mouse_y, w->x, w->y, w->w, w->h)) continue;
                        focused = i; bring_to_front(i);
                        if(pt_in(mouse_x, mouse_y, btn_x(w,0), btn_y(w), BTN_W, BTN_H)) w->state = 1;
                        else if(pt_in(mouse_x, mouse_y, btn_x(w,1), btn_y(w), BTN_W, BTN_H)) toggle_max(i);
                        else if(pt_in(mouse_x, mouse_y, btn_x(w,2), btn_y(w), BTN_W, BTN_H)) w->state = 1;
                        else if(pt_in(mouse_x, mouse_y, w->x, w->y, w->w, TITLE_H)){
                            begin_drag(i);
                            ui_dirty = 0;
                        }
                        else if(i == 3) handle_calc_click(mouse_x, mouse_y);
                        else if(i == 2) handle_notepad_click(mouse_x, mouse_y);
                        else if(i == 4) handle_files_click(mouse_x, mouse_y);
                        if(!handled) handled = 1;
                        break;
                    }
                }
            }
            if(right_edge){
                if(start_open) start_open = 0;
                else if(ctx_open) ctx_open = 0;
                else {
                    int over_win = 0;
                    for(int k = WIN_COUNT-1; k >= 0; k--){
                        int i = zorder[k];
                        if(wins[i].state == 1) continue;
                        if(pt_in(mouse_x, mouse_y, wins[i].x, wins[i].y, wins[i].w, wins[i].h)){ over_win = 1; break; }
                    }
                    if(!over_win){ ctx_open = 1; ctx_x = mouse_x; ctx_y = mouse_y; }
                }
            }
            if(release){
                if(drag_win >= 0){
                    drag_win = -1;
                    ui_dirty = 1;
                }
            }
            if(drag_win >= 0 && left_now){
                drag_step();
                cursor_transition(cur_sx, cur_sy, mouse_x, mouse_y);
                fps_frames++;
                last_left = left_now;
                last_right = right_now;
                continue;
            }
            last_left = left_now;
            last_right = right_now;
        }

        if(ui_dirty){
            cur_sx = -1;
            draw_desktop();
            blit_full();
            cursor_transition(-1, -1, mouse_x, mouse_y);
            fps_frames++;
        } else if(cursor_moved){
            cursor_transition(cur_sx, cur_sy, mouse_x, mouse_y);
            fps_frames++;
        }
    }
}
