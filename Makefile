# Enable kbuild verbose mode:
# $ make V=1

KSRC ?= /lib/modules/$(shell uname -r)/build

EXTRA_CFLAGS += -Wall
DIST_VERSION = 1.01

CONFIG_SHUTTLE_VFD := m

obj-$(CONFIG_SHUTTLE_VFD)	+= shuttle_vfd.o

all:
	$(MAKE) -C $(KSRC) M=$(PWD) modules

clean:
	$(MAKE) -C $(KSRC) M=$(PWD) clean

distclean: clean
	rm -rf cscope.* *~

dist: Makefile shuttle_vfd.c
	@tar -cf - $< | gzip -9 > shuttle_vfd_driver-$(DIST_VERSION).tar.gz
