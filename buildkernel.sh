#! /bin/bash

echo "building kernel...................."
cp arch/arm/configs/z2440_defconfig .config
make uImage

echo "building done!"
