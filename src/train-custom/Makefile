CROSS_COMPILE = xtensa-esp32-elf-
CC = $(CROSS_COMPILE)gcc
AS = $(CROSS_COMPILE)as
LD = $(CROSS_COMPILE)ld
OBJCOPY = $(CROSS_COMPILE)objcopy
SIZE = $(CROSS_COMPILE)size

CFLAGS = -nostdlib -fno-builtin -Os -g3 \
         -ffunction-sections -fdata-sections \
         -mlongcalls -DESP_PLATFORM -mtext-section-literals \
         -Wall -Werror -Wno-error=unused-function \
         -std=gnu99

LDFLAGS = -nostdlib -T esp32_linker.ld \
          -Wl,-static -Wl,--gc-sections \
          -u call_user_start_cpu0 \
          -Wl,-EL -Wl,-Map=esp32_train.map

INC_FLAGS = -I./include

SRC_C = src/boot.c src/libc.c src/netstack.c \
        src/irq.c src/malloc.c src/train_control.c
SRC_S = src/vectors.S

OBJ_C = $(SRC_C:.c=.o)
OBJ_S = $(SRC_S:.S=.o)
OBJS = $(OBJ_C) $(OBJ_S)

TARGET = esp32_train
BIN = $(TARGET).bin
ELF = $(TARGET).elf

QEMU = qemu-system-xtensa
QEMU_FLAGS = -machine esp32 -nographic \
             -serial mon:stdio \
             -drive file=flash.bin,if=mtd,format=raw \
             -nic user,model=open_eth,id=u1 \
             -global driver=timer.esp32.timg,property=wdt_disable,value=true

all: $(BIN) size

run: $(BIN)
	@echo "Running ESP32 train control system in QEMU..."
	$(QEMU) $(QEMU_FLAGS) -kernel $(BIN)

flash.bin: $(BIN)
	@echo "Creating flash image for QEMU..."
	dd if=/dev/zero of=flash.bin bs=1M count=4
	dd if=$(BIN) of=flash.bin conv=notrunc

$(BIN): $(ELF)
	@echo "Generating binary $@..."
	$(OBJCOPY) -O binary $< $@

$(ELF): $(OBJS) esp32_linker.ld
	@echo "Linking $@..."
	$(CC) $(LDFLAGS) -o $@ $(OBJS)

%.o: %.c
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) $(INC_FLAGS) -c $< -o $@

%.o: %.S
	@echo "Assembling $<..."
	$(CC) $(CFLAGS) $(INC_FLAGS) -c $< -o $@

size: $(ELF)
	$(SIZE) -A $(ELF)

clean:
	@echo "Cleaning..."
	rm -f $(OBJS) $(BIN) $(ELF) $(TARGET).map flash.bin

esp32_linker.ld:
	@echo "Creating ESP32 linker script..."
	@echo "/* ESP32 Linker Script */" > $@
	@echo "MEMORY {" >> $@
	@echo "  IRAM : ORIGIN = 0x40080000, LENGTH = 0x20000" >> $@
	@echo "  DRAM : ORIGIN = 0x3FFF0000, LENGTH = 0x10000" >> $@
	@echo "}" >> $@
	@echo "SECTIONS {" >> $@
	@echo "  .text : { *(.vectors) *(.text) *(.text.*) } > IRAM" >> $@
	@echo "  .rodata : { *(.rodata) *(.rodata.*) } > DRAM" >> $@
	@echo "  .data : { *(.data) *(.data.*) } > DRAM" >> $@
	@echo "  .bss : { *(.bss) *(.bss.*) *(COMMON) } > DRAM" >> $@
	@echo "}" >> $@

.PHONY: all run clean size
