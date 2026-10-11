# MyOS

A 32-bit operating system built from scratch.

## Features
- GRUB Multiboot bootloader
- Paging (4 MB pages)
- GDT, IDT, PIC remap, PIT 100 Hz
- PS/2 keyboard and mouse
- Linear framebuffer 800x600x32
- Ring 3 user mode with syscall gate
- ELF32 loader
- Preemptive round-robin scheduler
- Window manager (drag, minimize, taskbar)
- In-memory filesystem
- Scripting engine (MyScript)

## Build
    sudo apt install nasm gcc-multilib grub-pc-bin xorriso mtools qemu-system-x86
    make
    make run

## License
MIT
