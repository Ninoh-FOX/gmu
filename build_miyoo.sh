#!/bin/bash

set -ex

make clean

CC="/opt/miyoomini-toolchain/usr/bin/arm-linux-gnueabihf-gcc" LFLAGS="-lpthread" ./configure --libs=/opt/miyoomini-toolchain/usr/arm-miyoomini-linux-gnueabihf/sysroot/usr/lib/arm-linux-gnueabihf --includes=/opt/miyoomini-toolchain/usr/arm-miyoomini-linux-gnueabihf/sysroot/usr/include --target-device=miyoo --disable=notify-frontend

make
