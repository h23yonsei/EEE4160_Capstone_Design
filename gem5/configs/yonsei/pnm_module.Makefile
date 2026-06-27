obj-m := pnm_module.o

KDIR ?= /lib/modules/6.8.0-52-generic/build  # pinned to the guest kernel inside the disk image

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
