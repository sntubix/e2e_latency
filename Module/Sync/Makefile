CROSS_COMPILE ?= aarch64-linux-gnu-
ARCH ?= arm64

obj-m += gpio16_irq.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean