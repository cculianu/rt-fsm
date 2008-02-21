TARGETS=GetMatrix GetValue ProbeComedi SendMatrix
CFLAGS=-W -Wall
CXXFLAGS=-W -Wall
CC=gcc
CXX=g++

all: $(TARGETS)

ProbeComedi: ProbeComedi.o
	$(CXX) $(LDFLAGS) -o $@ $< -lcomedi

ProbeComedi.o: ProbeComedi.cpp
	$(CXX) $(CXXFLAGS) -c $<

clean:
	-rm -f *~ $(TARGETS) *.o
