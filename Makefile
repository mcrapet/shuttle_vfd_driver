# Enable kbuild verbose mode:
# $ make V=1

KSRC ?= /lib/modules/$(shell uname -r)/build

EXTRA_CFLAGS += -Wall

CONFIG_SHUTTLE_VFD := m

obj-$(CONFIG_SHUTTLE_VFD)	+= shuttle_vfd.o

all:
	$(MAKE) -C $(KSRC) M=$(PWD) modules

clean:
	rm -rf *.ko *.mod.* *.o .*.o.d .*.cmd .tmp_versions Module.symvers *.order

distclean: clean
	rm -rf cscope.* *~

