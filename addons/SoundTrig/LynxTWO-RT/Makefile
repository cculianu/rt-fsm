
SRCDIR = LynxTWO
MOD = LynxTWO.o
CURRENT = $(shell uname -r)
KDIR = /lib/modules/$(CURRENT)/build
PWD = $(shell pwd)

all: THIS MOD

THIS:
	make -C $(SRCDIR)  && [ -f $(SRCDIR)/$(MOD) ] && cp -f $(SRCDIR)/$(MOD) $(MOD)_shipped

MOD:
	make -C $(KDIR) M=$(PWD) modules

clean:
	rm -f *.o *~ Makefile.bak *\# *.o_shipped
	make -C $(SRCDIR) clean
	make -C $(KDIR) M=$(PWD) clean

