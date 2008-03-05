
SRCDIR = LynxTWO
MOD = LynxTWO.o
CURRENT = $(shell uname -r)
KDIR = /lib/modules/$(CURRENT)/build
PWD = $(shell pwd)

all: THIS MOD

THIS:
	make -C $(SRCDIR)  && [ -f $(SRCDIR)/$(MOD) ] && cp -f $(SRCDIR)/$(MOD) $(MOD)_shipped

MOD:
	@-if test -e $(KDIR)/Module.symvers; then mv -f $(KDIR)/Module.symvers $(KDIR)/__Module.symvers; fi; if test -e $(KDIR)/vmlinux; then mv -f $(KDIR)/vmlinux $(KDIR)/__vmlinux; fi
	make -C $(KDIR) M=$(PWD) modules
	@-if test -e $(KDIR)/__Module.symvers; then mv -f $(KDIR)/__Module.symvers $(KDIR)/Module.symvers; fi; if test -e $(KDIR)/__vmlinux; then mv -f $(KDIR)/__vmlinux $(KDIR)/vmlinux; fi


clean:
	rm -f *.o *~ Makefile.bak *\# *.o_shipped
	make -C $(SRCDIR) clean
	make -C $(KDIR) M=$(PWD) clean

