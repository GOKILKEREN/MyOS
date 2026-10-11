CC      = gcc
CFLAGS  = -m32 -ffreestanding -fno-pie -fno-pic -fno-stack-protector -fno-builtin \
          -mno-sse -mno-mmx -mno-80387 -mno-fp-ret-in-387 \
          -mpreferred-stack-boundary=2 -fcf-protection=none \
          -Wall -Wextra -O2 -c
UCFLAGS = -m32 -ffreestanding -fno-pie -fno-pic -nostdlib -nostartfiles \
          -fno-stack-protector -mno-sse -mno-mmx -O2 -c
LD      = ld
LDFLAGS = -m elf_i386 -T linker.ld

# user program discovery
USER_SRCS  = $(wildcard user/*.c)
USER_OBJS  = $(patsubst user/%.c,user_build/%.o,$(USER_SRCS))
USER_ELFS  = $(patsubst user/%.c,user_build/%.elf,$(USER_SRCS))

OBJS = boot.o isr.o int80.o switch.o scheduler.o syscall.o idt.o font.o paging.o gdt.o myscript.o user_prog.o ring3.o elf.o user_programs.o kernel.o

all: myos.iso

# kernel asm/c
%.o: %.asm
	nasm -f elf32 $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) $< -o $@

# user programs
user_build:
	mkdir -p user_build

user_build/%.o: user/%.c user/lib.h | user_build
	$(CC) $(UCFLAGS) -Iuser $< -o $@

user_build/%.elf: user_build/%.o user/linker.ld
	$(LD) -m elf_i386 -T user/linker.ld -o $@ $<

# auto-embed generator
user_programs.c: $(USER_ELFS) user/gen_embed.py
	python3 user/gen_embed.py $(USER_ELFS) > $@

user_programs.o: user_programs.c user_programs.h
	$(CC) $(CFLAGS) user_programs.c -o $@

# link kernel
kernel.elf: $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

myos.iso: kernel.elf
	mkdir -p iso/boot/grub
	cp kernel.elf iso/boot/kernel.elf
	printf 'set timeout=0\nset default=0\nmenuentry "MyOS" {\n  multiboot /boot/kernel.elf\n  boot\n}\n' > iso/boot/grub/grub.cfg
	grub-mkrescue -o myos.iso iso

run: myos.iso
	qemu-system-i386 -cdrom myos.iso

clean:
	rm -f *.o *.elf *.iso user_programs.c
	rm -rf iso user_build

.PHONY: all run clean
