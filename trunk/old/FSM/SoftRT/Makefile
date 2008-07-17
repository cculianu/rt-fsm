

SRC_C = RatExpFSM.c softtask.c rtfemul.c mbuffemul.cpp
SRC_CXX = RatExpFSMServer.cpp rtos_utility.cpp
PRG_OBJS = RatExpFSMServer.o rtos_utility.o rtfemul.o
MOD_OBJS = RatExpFSM.o softtask.o rtfemul.o mbuffemul.o
MOD = RatExpFSM
PRG = RatExpFSMServer
CXX=g++
CFLAGS=-W -Wall -g
CXXFLAGS=-W -Wall -g

.c.o:
	$(CC) $(CFLAGS) -c  $<

.cpp.o:
	$(CXX) $(CXXFLAGS) -c $<

all: .deps $(MOD) $(PRG) 

$(MOD): $(MOD_OBJS)
	g++ -o $(MOD) $(MOD_OBJS) -lrt -lcomedi -lpthread

$(PRG): $(PRG_OBJS)
	$(CXX) $(CXXFLAGS) -lpthread -o $@ $(PRG_OBJS)

.deps: 
	-makedepend -- -- $(SRC_C) $(SRC_CXX)
	touch .deps

clean:
	rm -f *~ *.o Makefile.bak .deps $(PRG) $(MOD)

