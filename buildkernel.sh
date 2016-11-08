#! /bin/bash

echo "building kernel...................."
if [ ! -d ../rootfs ]; then
	tar xvjf rootfs.tar.bz2 -C ../ 
fi

if [ ! -f ".config" ]; then
	cp arch/arm/configs/z2440_defconfig .config
fi

make uImage

echo "building done!"
