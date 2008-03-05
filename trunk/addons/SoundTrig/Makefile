SRC = SoundServer.cpp rtos_utility.cpp WavFile.cpp Util.cpp
PRG_OBJS = SoundServer.o rtos_utility.o WavFile.o Util.o
PRG = SoundServer

CXX=g++
CXXFLAGS=-W -Wall -g -I../../include -I ../UserspaceExtTrig -I ../../user
CURRENT = $(shell uname -r)
KDIR = /lib/modules/$(CURRENT)/build
PWD = $(shell pwd)

.cpp.o:
	$(CXX) $(CXXFLAGS) -c $<

all: .deps MOD $(PRG) DRV

MOD: 
	@-if test -e $(KDIR)/Module.symvers; then mv -f $(KDIR)/Module.symvers $(KDIR)/__Module.symvers; fi; if test -e $(KDIR)/vmlinux; then mv -f $(KDIR)/vmlinux $(KDIR)/__vmlinux; fi
	make -C $(KDIR) M=$(PWD) modules
	@-if test -e $(KDIR)/__Module.symvers; then mv -f $(KDIR)/__Module.symvers $(KDIR)/Module.symvers; fi; if test -e $(KDIR)/__vmlinux; then mv -f $(KDIR)/__vmlinux $(KDIR)/vmlinux; fi
	

DRV:
	make -C $(PWD)/LynxTWO-RT 
		
rtos_utility.o: ../../user/rtos_utility.cpp
	$(CC) $(CXXFLAGS) -c -o $@ $<

Util.o: ../../user/Util.cpp
	$(CC) $(CXXFLAGS) -c -o $@ $<

$(PRG): $(PRG_OBJS)
	$(CXX) $(CXXFLAGS) -lpthread -o $@ $(PRG_OBJS)

.deps: 
	-cp -f Makefile.bak Makefile || true
	-makedepend -- $(CXXFLAGS) --  $(SRC) 2> mkdep.out || cat mkdep.out 1>&2
	-rm -f mkdep.out	
	touch .deps

clean:
	-cp -f Makefile.bak Makefile || true
	rm -f *~ *.o Makefile.bak .deps $(PRG)
	make -C $(KDIR) M=$(PWD) clean
	make -C $(PWD)/LynxTWO-RT clean
	


