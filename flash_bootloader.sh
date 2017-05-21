#!/bin/bash

cd muc-loader
openocd -f board/moto_mdk_muc_reset.cfg -c "program ./out/boot_hdk.bin 0x08000000 reset exit"
