#!/bin/sh
# script to build a binary IPL and boot image for the  OMAP3530 Beagle board
set -v
# Convert IPL header into Binary format
${QNX_HOST}/usr/bin/ntoarmv7-objcopy --input-format=elf32-littlearm --output-format=binary ../src/hardware/ipl/boards/omap3530beagle/arm/le.v7/boot_header.o ./tmp-boot-header.bin

# Convert IPL into Binary format
${QNX_HOST}/usr/bin/ntoarmv7-objcopy --input-format=elf32-littlearm --output-format=binary ../src/hardware/ipl/boards/omap3530beagle/arm/le.v7/ipl-omap3530beagle ./tmp-ipl-omap3530beagle.bin

# Cat boot header and ipl together
cat ./tmp-boot-header.bin ./tmp-ipl-omap3530beagle.bin > ./tmp-header-ipl-omap3530beagle.bin

# Pad Binary IPL to 24K image, this is the image used by boot from UART
mkrec -s24k -ffull -r ./tmp-ipl-omap3530beagle.bin > ./ipl-omap3530beagle.bin

# Pad Binary IPL with Header to 24K image, this is the image to put on NAND and boot from NAND
mkrec -s24k -ffull -r ./tmp-header-ipl-omap3530beagle.bin > ./nand-ipl-omap3530beagle.bin

# clean up temporary files
rm -f tmp*.bin

echo "done!!!!!!!"
