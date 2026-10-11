#include <stdint.h>

typedef struct {
    uint32_t esp;
    int state;
    int is_user;
} task_t;

#define TASK_MAX    4
#define STACK_SIZE  8192

static task_t  tasks[TASK_MAX];
static uint8_t stacks[TASK_MAX][STACK_SIZE] __attribute__((aligned(16)));
static int     task_count = 0;
static int     current_task = 0;

extern void task_switch_to(uint32_t* save_esp, uint32_t new_esp);
extern void tss_set_kernel_stack(uint32_t esp0);

void scheduler_init(void){
    for(int i = 0; i < TASK_MAX; i++){
        tasks[i].state = 0;
        tasks[i].esp = 0;
        tasks[i].is_user = 0;
    }
    tasks[0].state = 2;
    task_count = 1;
    current_task = 0;
}

static int task_create_user(uint32_t entry, uint32_t user_stack_top){
    if(task_count >= TASK_MAX) return -1;
    int idx = task_count++;
    task_t* t = &tasks[idx];

    uint32_t* sp = (uint32_t*)(stacks[idx] + STACK_SIZE);

    /* IRQ-style frame (low to high):
       [pusha 32B][int_no 4B][err_code 4B][EIP][CS][EFLAGS][ESP_user][SS_user] */
    *--sp = 0x23;              /* SS  = user data */
    *--sp = user_stack_top;    /* ESP user */
    *--sp = 0x202;             /* EFLAGS (IF=1) */
    *--sp = 0x1B;              /* CS  = user code */
    *--sp = entry;             /* EIP */
    *--sp = 0;                 /* err_code */
    *--sp = 0;                 /* int_no */
    for(int i = 0; i < 8; i++) *--sp = 0;   /* pusha */

    t->esp     = (uint32_t)sp;
    t->state   = 1;
    t->is_user = 1;
    return idx;
}

void task_spawn_user(uint32_t entry, uint32_t user_stack_top){
    int idx = task_create_user(entry, user_stack_top);
    if(idx < 0) return;

    int prev = current_task;
    if(tasks[prev].state == 2) tasks[prev].state = 1;
    tasks[idx].state = 2;
    current_task = idx;

    tss_set_kernel_stack((uint32_t)(stacks[idx] + STACK_SIZE));
    task_switch_to(&tasks[prev].esp, tasks[idx].esp);
}

void task_exit(void){
    tasks[current_task].state = 0;
    int prev = current_task;
    current_task = 0;
    tasks[0].state = 2;
    tss_set_kernel_stack((uint32_t)(stacks[0] + STACK_SIZE));
    task_switch_to(&tasks[prev].esp, tasks[0].esp);
    for(;;) __asm__ volatile("cli;hlt");
}

/* Called from IRQ0 handler in kernel.c.
   cur_esp = pointer to pushed regs on current task's kernel stack.
   Returns new esp (another task's) to switch to, or 0 = no switch. */
uint32_t scheduler_tick(uint32_t cur_esp){
    if(task_count <= 1) return 0;

    int cur = current_task;

    int next = -1;
    for(int i = 1; i <= TASK_MAX; i++){
        int idx = (cur + i) % TASK_MAX;
        if(idx == cur) continue;
        if(tasks[idx].state == 1){ next = idx; break; }
    }
    if(next < 0) return 0;

    tasks[cur].esp = cur_esp;
    if(tasks[cur].state == 2) tasks[cur].state = 1;

    tasks[next].state = 2;
    current_task = next;
    tss_set_kernel_stack((uint32_t)(stacks[next] + STACK_SIZE));

    return tasks[next].esp;
}
