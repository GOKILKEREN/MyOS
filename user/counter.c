#include "lib.h"

void _start(void){
    banner("counter");
    for(int i = 0; i <= 20; i += 2){
        sys_print("counter = ");
        put_intln(i);
    }
    sys_print("\n[counter] done\n");
    sys_exit();
}
