obj-m := thermaltake-psu.o

# Everything below is for running make directly; kbuild only needs obj-m.
ifeq ($(KERNELRELEASE),)

KDIR ?= /lib/modules/$(shell uname -r)/build
DIST_VERSION := $(shell sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' dkms.conf)
DIST_NAME := thermaltake-psu-$(DIST_VERSION)
DIST_FILES := thermaltake-psu.c Makefile dkms.conf LICENSE README.md docs/thermaltake-psu.rst

all: modules

modules:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

install: modules
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules_install
	depmod -a

# Driver-only source tarball for DKMS, built from the v$(DIST_VERSION) git tag.
dist:
	git archive --format=tar.gz --prefix=$(DIST_NAME)/ -o $(DIST_NAME).tar.gz \
		v$(DIST_VERSION) $(DIST_FILES)
	sha256sum $(DIST_NAME).tar.gz > $(DIST_NAME).tar.gz.sha256

.PHONY: all modules clean install dist

endif
