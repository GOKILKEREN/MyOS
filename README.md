# MyOS

A 32-bit operating system built from scratch — kernel, GUI, user mode,
and everything in between.

**Vibecoded.** This project is written through AI-assisted development
(vibecoding). The architecture, design decisions, and debugging are
directed by a human; the code is generated and iterated with AI help.
The goal is not to hide that — it is to show what a solo developer can
build in a few sessions with the right tools.

## Features

- **Bootloader**: GRUB Multiboot, 32-bit ELF
- **Memory**: Paging with 4 MB pages (PSE), identity mapped
- **Protection**: GDT, IDT, TSS, ring 0 / ring 3 separation
- **Interrupts**: PIC remap, PIT at 100 Hz, IRQ0-15
- **Input**: PS/2 keyboard (full US layout, shift, caps, numpad) and PS/2 mouse (IRQ12)
- **Graphics**: Linear framebuffer 800x600x32, double buffered
- **User mode**: Ring 3, syscall gate via `int 0x80`
- **ELF loader**: Runs user programs compiled separately with GCC
- **Scheduler**: Preemptive round-robin, context switch in assembly
- **Filesystem**: In-memory FS with basic create/read/write/delete
- **Window manager**: Multiple windows, drag, minimize, maximize, taskbar
- **Scripting**: MyScript — custom scripting language (vars, loops, functions, built-ins)
- **User libc**: Minimal libc with syscall wrappers, auto-embedded programs

## Build Requirements

    sudo apt install nasm gcc-multilib grub-pc-bin grub-common xorriso mtools qemu-system-x86

## Build and Run

    make
    make run

## What It Looks Like

- Blue gradient wallpaper
- Window manager with taskbar, clock, start menu, context menu
- Desktop icons: Terminal, System Info, Notepad, Calculator, File Manager
- Terminal window with shell commands
- User-mode GUI applications (window, buttons, mouse events)

## User Programs

Drop a `.c` file into `user/`, rebuild, and it is auto-compiled, linked
as ELF32, and embedded into the kernel image.

Example `user/hello.c`:

    #include "lib.h"

    void _start(void){
        sys_print("Hello from userspace!\n");
        sys_exit();
    }

Then from the MyOS shell:

    exec HELLO.ELF

## Project Structure
    boot.asm        GRUB multiboot entry
    isr.asm         IRQ stubs
    int80.asm       Syscall stub
    switch.asm      Context switch (task_switch_to)
    idt.c           Interrupt descriptor table
    gdt.c           GDT and TSS
    paging.c        Page directory, enable paging
    syscall.c       Syscall dispatcher
    scheduler.c     Round-robin preemptive scheduler
    elf.c           ELF32 loader
    font.c          8x8 bitmap font
    myscript.c      Scripting language interpreter
    ring3.c         User mode entry
    kernel.c        Main kernel, drivers, GUI
    user/           User programs (auto-embedded)

## Status

Work in progress. Currently supports a functional desktop environment,
ring 3 user programs, ELF loading, and preemptive multitasking.

Known issues:
- Exec'ing a user program halts the kernel after exit (will be fixed
  once scheduler reclaims the kernel task cleanly)
- Some character glyphs not mapped (special symbols)

## Vibecoding Note

This project is a demonstration of vibecoding in a hard domain. OS
development is traditionally a multi-year effort for a team. MyOS was
built in a handful of sessions with AI assistance, driven by a single
developer directing the design and validating every step.

The point is not that "AI wrote an OS". The point is that a determined
solo developer, with the right tools, can now go from zero to a working
kernel with user mode and a GUI in a much shorter time than before.

## License

MIT
