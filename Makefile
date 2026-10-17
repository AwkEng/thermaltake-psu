obj-m := thermaltake-psu.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all: modules

modules:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

install: modules
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules_install
	depmod -a

.PHONY: all modules clean install
