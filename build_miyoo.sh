#!/bin/bash

set -ex

make clean

CC="/usr/bin/arm-linux-gnueabihf-gcc" LFLAGS="-lpthread" ./configure --libs=/usr/lib/arm-linux-gnueabihf --includes=/usr/include/arm-linux-gnueabihf --target-device=miyoo --disable=notify-frontend

make
