CURRENT = $(shell uname -r)
KDIR = /lib/modules/$(CURRENT)/build
TARGET = RealtimeFSM
OBJS = fsm.o softtask.o deflate_helper.o extra_mathfuncs.o float_vsnprintf.o 
MDIR = rtai
RTAIDIR = /usr/realtime
COMEDIDIR = /usr/src/comedi
PWD = $(shell pwd)
DEST = /lib/modules/$(CURRENT)/kernel/$(MDIR)
EXTRA_CFLAGS += -DEXPORT_SYMTAB $(shell $(RTAIDIR)/bin/rtai-config --module-cflags)  -I$(src)/../include -I$(COMEDIDIR)/include -I/usr/include -DRTAI
MACH = $(shell uname -m)

ifeq ($(MACH), x86_64)
# workaround for kernel build system sometimes addind -mno-see and -mno-sse2 which messes up our
# use of floats in the kernel
EXTRA_CFLAGS += -msse -msse2
endif

ifeq ($(KERNELRELEASE),)

all: 
		@-if test -e $(KDIR)/Module.symvers; then mv -f $(KDIR)/Module.symvers $(KDIR)/__Module.symvers; fi; if test -e $(KDIR)/vmlinux; then mv -f $(KDIR)/vmlinux $(KDIR)/__vmlinux; fi
		make -C $(KDIR) M=$(PWD) modules
		@-if test -e $(KDIR)/__Module.symvers; then mv -f $(KDIR)/__Module.symvers $(KDIR)/Module.symvers; fi; if test -e $(KDIR)/__vmlinux; then mv -f $(KDIR)/__vmlinux $(KDIR)/vmlinux; fi

install:
	su -c "cp -v $(TARGET).ko $(DEST) && /sbin/depmod -a"

clean:
	-rm -f *.o *.ko .*.cmd .*.flags *.mod.c *~
	make -C $(KDIR) M=$(PWD) clean
	

else	

obj-m      := $(TARGET).o

$(TARGET)-objs := $(OBJS)

-include $(KDIR)/Rules.make

endif
