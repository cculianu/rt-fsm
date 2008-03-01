
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
          HalDMA.cpp      HalMIDIDevice.cpp  HalRegister.cpp   HalWaveDevice.cpp
MOD_OBJS = \
	  LinuxGlue.o \
          Hal4114.o     HalDevice.o      HalMixer.o      HalSampleClock.o \
          Hal8420.o     HalEEPROM.o      HalPlayMix.o    HalTimecode.o \
          HalAdapter.o  HalLStream.o     HalRecordMix.o  HalWaveDMADevice.o \
          HalDMA.o      HalMIDIDevice.o  HalRegister.o   HalWaveDevice.o 
          
ifneq ($(MACH),x86_64)
# regular x86?
MOD_OBJS +=  _divdi3.o _udivdi3.o
CXXFLAGS += -DARCH=x86

else
# x86_64
CXXFLAGS += -mcmodel=kernel -DARCH=x86_64

endif

CXX=g++
CXXFLAGS += -W -Wall -fno-rtti -nostartfiles -nostdinc -fno-exceptions -nostdinc++ -fms-extensions -fno-pic -fno-PIC -DLINUX -DRTAI  $(DEBUG) -I../ -I. -I../Include
TARGET=LynxTWO.o

.cpp.o:
	$(CXX) $(CXXFLAGS) -c -o $@ $<

all: .deps $(TARGET) 

.deps: 
	-makedepend -- $(CXXFLAGS) -- $(SRC_CXX)
	touch .deps

_divdi3.o:
	ar x `gcc  --print-libgcc-file-name` _divdi3.o
	
_udivdi3.o:
	ar x `gcc  --print-libgcc-file-name` _udivdi3.o

clean:
	cp -f Makefile.bak Makefile 2>/dev/null || true 
	rm -f *~ *.o *.oS Makefile.bak \* .deps 

$(TARGET): $(MOD_OBJS) 
	$(LD) -r -o $(TARGET) $(MOD_OBJS) 

