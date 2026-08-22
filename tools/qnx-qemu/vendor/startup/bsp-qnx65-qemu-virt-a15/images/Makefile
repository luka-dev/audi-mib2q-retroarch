ROOT_DIR := $(notdir $(CURDIR))
ifndef QCONFIG
QCONFIG=qconfig.mk
endif
include $(QCONFIG)

HOST_MKIFS := mkifs

SUFFIXES := .build .ifs .bin

.PHONY: all clean

all: bsp-qemu-virt.bin 

clean: 
	$(RM_HOST) bsp-qemu-virt.bin 

bsp-qemu-virt.bin: virt.build
	$(HOST_MKIFS) -r../install -v $(MKIFSFLAGS) $^ $@
