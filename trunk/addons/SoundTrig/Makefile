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

all: .deps MOD $(PRG)

MOD: 
	make -C $(KDIR) M=$(PWD) modules
	
rtos_utility.o: ../../user/rtos_utility.cpp
	$(CC) $(CXXFLAGS) -c -o $@ $<

Util.o: ../../user/Util.cpp
	$(CC) $(CXXFLAGS) -c -o $@ $<

$(PRG): $(PRG_OBJS)
	$(CXX) $(CXXFLAGS) -lpthread -o $@ $(PRG_OBJS)

.deps: 
	-cp -f Makefile.bak Makefile || true
	-makedepend -- $(CXXFLAGS) --  $(SRC)
	touch .deps

clean:
	-cp -f Makefile.bak Makefile || true
	rm -f *~ *.o Makefile.bak .deps $(PRG)
	make -C $(KDIR) M=$(PWD) clean
	


