name := clevofan
obj-m := $(name).o

KVERSION := $(shell uname -r)
KDIR := /lib/modules/$(KVERSION)/build
PWD := $(shell pwd)

ifdef DEBUG
CFLAGS_$(obj-m) := -DDEBUG
endif

default:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) O=$(PWD) -C $(KDIR) M=$(PWD) clean

load:
	-rmmod $(name)
	insmod $(name).ko

install:
	install -m 0755 -o root -g root $(name).ko /lib/modules/$(KVERSION)/updates/$(name).ko
	depmod -a

uninstall:
	rm /lib/modules/$(KVERSION)/updates/$(name).ko
	depmod -a
