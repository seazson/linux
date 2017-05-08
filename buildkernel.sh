#! /bin/bash

echo "building kernel...................."
if [ ! -d ../rootfs ]; then
	echo "extra rootfs"
	tar xvjf rootfs.tar.bz2 -C ../ 
fi

if [ ! -f ".config" ]; then
	echo "no config"
	cp arch/arm/configs/z2440_defconfig .config
fi

make uImage

echo "building done!"
