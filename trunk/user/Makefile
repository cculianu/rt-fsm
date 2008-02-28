SRC = FSMServer.cpp rtos_utility.cpp Util.cpp
PRG_OBJS = FSMServer.o rtos_utility.o scanproc.o deflate_helper.o Util.o
PRG = FSMServer
CXX=g++
CXXFLAGS=-W -Wall -g -I../include

.cpp.o:
	$(CXX) $(CXXFLAGS) -c $<

all: .deps $(PRG) 

scanproc.o: scanproc.c
	$(CC) $(CXXFLAGS) -c $<

deflate_helper.o: ../kernel/deflate_helper.c
	$(CC) $(CXXFLAGS) -c -o $@ $<

$(PRG): $(PRG_OBJS)
	$(CXX) $(CXXFLAGS) -lz -lpthread -o $@ $(PRG_OBJS)

.deps: 
	-cp -f Makefile.bak Makefile || true
	-makedepend -- $(CXXFLAGS) --  $(SRC)
	touch .deps

clean:
	-cp -f Makefile.bak Makefile || true
	rm -f *~ *.o Makefile.bak .deps $(PRG) $(MOD)


