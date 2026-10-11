#include "gui.h"

void _start(void){
    gui_create("GUI Demo", 200, 100, 400, 300);
    gui_clear(COLOR_LIGHT);

    gui_text(20, 20, "Halo dari userspace!", 0x00101010);
    gui_text(20, 40, "Klik tombol di bawah.", 0x00505050);

    /* tombol */
    gui_rect(20, 80, 160, 40, COLOR_BLUE);
    gui_text(60, 95, "Klik Aku", COLOR_WHITE);

    gui_flush();

    int clicks = 0;
    for(;;){
        gui_event_t ev;
        gui_wait_event(&ev);

        if(ev.type == EV_CLOSE) break;

        if(ev.type == EV_MOUSE && ev.btn == 1){
            /* klik di area tombol? */
            if(ev.x >= 20 && ev.x < 180 && ev.y >= 80 && ev.y < 120){
                clicks++;
                /* update counter */
                gui_rect(20, 140, 360, 40, COLOR_LIGHT);
                gui_text(20, 150, "Clicks: ", 0x00101010);
                /* tampilkan angka sederhana */
                char buf[16]; int i = 0;
                int n = clicks;
                if(n == 0) buf[i++] = '0';
                else {
                    char tmp[12]; int j = 0;
                    while(n){ tmp[j++] = '0' + (n % 10); n /= 10; }
                    while(j) buf[i++] = tmp[--j];
                }
                buf[i] = 0;
                gui_text(80, 150, buf, COLOR_RED);
                gui_flush();
            }
        }
    }

    gui_destroy();
    sys_exit();
}
