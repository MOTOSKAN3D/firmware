#!/bin/bash

cd nuttx/nuttx
openocd -f board/moto_mdk_muc_reset.cfg -c "program nuttx.tftf 0x08008000 reset exit"
