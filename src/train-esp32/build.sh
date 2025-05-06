#!/bin/sh

idf.py -DRUNNING_IN_QEMU=1 build

python $IDF_PATH/components/esptool_py/esptool/esptool.py --chip esp32 \
   merge_bin -o train.bin \
   --flash_mode dio --flash_size 4MB \
   0x1000 build/bootloader/bootloader.bin \
   0x8000 build/partition_table/partition-table.bin \
   0x10000 build/train_control_system.bin

dd if=/dev/zero of=flash_image.bin bs=1M count=4
dd if=train.bin of=flash_image.bin conv=notrunc 
rm -f train.bin


/home/relue/Downloads/qemu/build/qemu-system-xtensa -machine esp32 -nographic \
    -drive file=flash_image.bin,if=mtd,format=raw \
    -global driver=wifi.esp32,property=debug,value=1 \
    -netdev user,id=net0,hostfwd=tcp::8100-:8100 \
    -rtc base=utc,clock=host
