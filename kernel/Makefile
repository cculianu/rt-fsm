CURRENT = $(shell uname -r)
KDIR = /lib/modules/$(CURRENT)/build
TARGET = RealtimeFSM
#OBJS = fsm.o softtask.o deflate_helper.o rt_math/rt_math.o
OBJS = fsm.o softtask.o deflate_helper.o extra_mathfuncs.o
MDIR = rtai
RTAIDIR = /usr/realtime
COMEDIDIR = /usr/src/comedi
PWD = $(shell pwd)
DEST = /lib/modules/$(CURRENT)/kernel/$(MDIR)
EXTRA_CFLAGS += -DEXPORT_SYMTAB $(shell $(RTAIDIR)/bin/rtai-config --module-cflags)  -I$(obj)/../include -I$(COMEDIDIR)/include -I/usr/include -DRTAI

ifeq ($(KERNELRELEASE),) 

all: 
		make -C $(KDIR) M=$(PWD) modules
install:
	su -c "cp -v $(TARGET).ko $(DEST) && /sbin/depmod -a"

clean:
	-rm -f *.o *.ko .*.cmd .*.flags *.mod.c *~
#	make -C rt_math clean

else	

obj-m      := $(TARGET).o

RealtimeFSM-objs := $(OBJS)

#$(KBUILD_EXTMOD)/rt_math/rt_math.o:
#	make -C '$(KBUILD_EXTMOD)'/rt_math CFLAGS='$(CFLAGS)' CC='$(CC)'

$(TARGET).o: $(OBJS)
	$(LD) $(LD_RFLAG) -r -o $@ $(OBJS)

-include $(KDIR)/Rules.make

endif
