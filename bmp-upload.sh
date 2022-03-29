#!/bin/bash

if [ $# -ne 1 ]
then
echo "USAGE: bmp.sh filename.elf"
echo "ip address or autoscan coming"
exit
return
fi

   if [[ $1 == *.bin ]]; then
       arm-none-eabi-objcopy -I binary -O elf32-little --change-section-address .data=0x08000000 $1 $1.elf
       arm-none-eabi-gdb -ex "set confirm off" -ex "set pagination off"  -ex "target extended-remote blackmagic:2022"  -ex "monitor swdp_scan" -ex "attach 1" -ex "file $(echo $1).elf" -ex "load $(echo $1).elf 0x08000000"  -ex "compare-sections" -ex "kill" $1.elf -ex "quit"
       rm $1.elf
       exit
       return
   fi

arm-none-eabi-gdb -ex "set confirm off" -ex "set pagination off"  -ex "target extended-remote blackmagic:2022"  -ex "monitor swdp_scan" -ex "attach 1" -ex "file $(echo $1)" -ex "load $(echo $1) 0x08000000"  -ex "compare-sections" -ex "kill" $1 -ex "quit"
