#!/bin/bash
export CC=xtensa-lx106-elf-gcc
make clean
cd components/esp32-espfs/mkespfsimage/
make clean
make
make
cd -
cp components/esp32-espfs/mkespfsimage/mkespfsimage build/esp32-espfs/mkespfsimage/
cd ESP8266_RTOS_SDK/tools/kconfig
CC=gcc make clean
CC=gcc make
cd -
CC=gcc make menuconfig
make

