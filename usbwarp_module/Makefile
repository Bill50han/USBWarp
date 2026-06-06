# SPDX-License-Identifier: GPL-2.0
#
# UsbWarp Virtual HCD kernel module
#
# Build against the running kernel:
#   make
#
# Build against a specific kernel:
#   make KDIR=/path/to/kernel/source
#
# Install:
#   sudo insmod usbwarp.ko [enable_iso=0] [max_pending=256] [debug=1]
#
# Clean:
#   make clean

obj-m := usbwarp.o

usbwarp-objs := usbwarp_pci.o  \
                usbwarp_ring.o \
                usbwarp_buf.o  \
                usbwarp_hcd.o  \
                usbwarp_poll.o \
                usbwarp_debugfs.o

# Extra include path for the shared protocol header.
ccflags-y := -I$(src)/../include -Wall

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

.PHONY: all clean
