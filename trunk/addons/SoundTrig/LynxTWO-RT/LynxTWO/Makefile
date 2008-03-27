
# comment this out to disable debug
DEBUG = -DDEBUG -g
#DEBUG = -DNDEBUG 
#DEBUG = -DDEBUG -g -DTIME_ISR
#DEBUG = -DNDEBUG -DTIME_ISR
MACH = $(shell uname -m)

SRC_CXX = \
	  LinuxGlue.cpp \
	  Hal4114.cpp     HalDevice.cpp      HalMixer.cpp      HalSampleClock.cpp \
          Hal8420.cpp     HalEEPROM.cpp      HalPlayMix.cpp    HalTimecode.cpp \
          HalAdapter.cpp  HalLStream.cpp     HalRecordMix.cpp  HalWaveDMADevice.cpp \
          HalDMA.cpp      HalMIDIDevice.cpp  HalRegister.cpp   HalStatusCodes.cpp HalWaveDevice.cpp
MOD_OBJS = \
	  LinuxGlue.o \
          Hal4114.o     HalDevice.o      HalMixer.o      HalSampleClock.o \
          Hal8420.o     HalEEPROM.o      HalPlayMix.o    HalTimecode.o \
          HalAdapter.o  HalLStream.o     HalRecordMix.o  HalWaveDMADevice.o \
          HalDMA.o      HalMIDIDevice.o  HalRegister.o   HalStatusCodes.o HalWaveDevice.o 
          
ifneq ($(MACH),x86_64)
# regular x86?
CXXFLAGS += -Dx86 -msoft-float -mpreferred-stack-boundary=2

else
# x86_64
CXXFLAGS += -mcmodel=kernel -Dx86_64 -mhard-float 

endif
CXX=g++
CXXFLAGS += -W -Wall -fno-rtti -nostartfiles -nostdinc -fno-exceptions -nostdinc++ -fms-extensions -fno-pic -fno-PIC -fomit-frame-pointer -DLINUX -DRTAI  $(DEBUG) -I../ -I. -I../Include
TARGET=LynxTWO.o

.cpp.o:
	$(CXX) $(CXXFLAGS) -c -o $@ $<

all: .deps $(TARGET) 

.deps: 
	-makedepend -- $(CXXFLAGS) --  $(SRC_CXX) 2> mkdep.out || cat mkdep.out 1>&2
	-rm -f mkdep.out
	touch .deps

clean:
	cp -f Makefile.bak Makefile 2>/dev/null || true 
	rm -f *~ *.o *.oS Makefile.bak \* .deps 

$(TARGET): $(MOD_OBJS) 
	$(LD) -r -o $(TARGET) $(MOD_OBJS) 

