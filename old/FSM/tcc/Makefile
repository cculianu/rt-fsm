include /usr/rtlinux/rtl.mk

MODULE_COMPILE_FLAGS := $(CFLAGS) 

SRC_C = i386-asm.c  tcc.c tccasm.c  test_tcc_module.c i386-gen.c  tcc_embedded_rtlinux.c  tccelf.c
OBJS = tcc.o tcc_embedded_rtlinux.o setjmp.o
TARGET = libtcc.o

.c.o:
	$(CC) ${MODULE_COMPILE_FLAGS} -c $<

.S.o:
	$(CC) ${MODULE_COMPILE_FLAGS} -c $<

all: .deps $(TARGET)

$(TARGET): $(OBJS)
	ld -r -o $(TARGET) $(OBJS)

.deps:
	-cp -f Makefile.bak Makefile || true
	-makedepend -- ${MODULE_COMPILE_FLAGS} $(SRC_C)
	touch .deps

clean:
	-cp -f Makefile.bak Makefile || true
	rm -f *~ *.o Makefile.bak .deps $(TARGET) $(OBJS)



