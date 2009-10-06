#!/bin/sh

#make -C /home/shmuma/work/kernel/src/linux-2.6.31.1/ M=`pwd`
make -C /lib/modules/`uname -r`/build M=`pwd`
