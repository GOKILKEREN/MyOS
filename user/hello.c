#include "lib.h"

void _start(void){
    banner("hello");
    sys_print("Program ini pakai user libc.\n");
    sys_print("Kode jadi jauh lebih pendek.\n\n");
    for(int i = 1; i <= 5; i++){
        sys_print("Baris ke-");
        put_intln(i);
    }
    sys_print("\n[hello] done\n");
    sys_exit();
}
