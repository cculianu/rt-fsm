CURRENT = $(shell uname -r)
KDIR = /lib/modules/$(CURRENT)/build
TARGET = UserspaceExtTrig
OBJS = module.o
MDIR = rtai
RTAIDIR = /usr/realtime
COMEDIDIR = /usr/src/comedi
PWD = $(shell pwd)
DEST = /lib/modules/$(CURRENT)/kernel/$(MDIR)
EXTRA_CFLAGS += -DEXPORT_SYMTAB $(shell $(RTAIDIR)/bin/rtai-config --module-cflags)  -I$(obj)/../../include -I$(COMEDIDIR)/include -I/usr/include -DRTAI

ifeq ($(KERNELRELEASE),) 

all: 
		make -C $(KDIR) M=$(PWD) modules
		
install:
	su -c "cp -v $(TARGET).ko $(DEST) && /sbin/depmod -a"

clean:
	-rm -f *.o *.ko .*.cmd .*.flags *.mod.c *~

else	

obj-m      := $(TARGET).o

UserspaceExtTrig-objs := $(OBJS)

-include $(KDIR)/Rules.make

endif
