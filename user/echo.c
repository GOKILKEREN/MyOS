#include "lib.h"

void _start(void){
    banner("echo");
    for(int i = 1; i <= 8; i++){
        sys_print("echo: baris ");
        put_intln(i);
    }
    sys_print("\n[echo] done\n");
    sys_exit();
}
