#include "lib.h"

void _start(void){
    banner("fibonacci");
    int a = 0, b = 1;
    for(int i = 0; i < 15; i++){
        sys_print("fib(");
        put_int(i);
        sys_print(") = ");
        put_intln(a);
        int t = a + b; a = b; b = t;
    }
    sys_print("\n[fib] done\n");
    sys_exit();
}
