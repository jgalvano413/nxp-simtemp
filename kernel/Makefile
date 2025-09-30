
KDIR ?= /lib/modules/$(shell uname -r)/build

obj-m += nxp_simtemp.o
nxp_simtemp-objs := src/nxp_simtemp.o

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
