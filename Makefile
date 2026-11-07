# kbuild reads obj-m from the Kbuild file, so this Makefile is only for running
# make directly or from DKMS. No KERNELRELEASE guard: DKMS 3.x passes
# KERNELRELEASE on the command line, which hid every target behind the guard.
KERNELRELEASE ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KERNELRELEASE)/build
DIST_VERSION := $(shell sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' dkms.conf)
DIST_NAME := thermaltake-psu-$(DIST_VERSION)
DIST_FILES := thermaltake-psu.c Kbuild Makefile dkms.conf LICENSE README.md docs/thermaltake-psu.rst

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
