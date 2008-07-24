
PWD = $(shell pwd)
PWD_ESC_SLASH = $(shell echo -n `pwd`/EmbC.build | sed '{s/\//\\\//g}')
all:    EmbC.build/mod.ko

clean:
	rm -fr EmbC.build *~ 

EmbC.build/Makefile_for_Kbuild.EmbC: Makefile_for_Kbuild.EmbC
	cat Makefile_for_Kbuild.EmbC | sed '{s/REPLACEME/$(PWD_ESC_SLASH)/g}' > EmbC.build/Makefile_for_Kbuild.EmbC

EmbC.build/mod_impl.c: EmbC.build
	echo "int foo = 1;" > EmbC.build/mod_impl.c

EmbC.build/mod_wrapper.c: EmbC.build embc_mod_wrapper.c
	cp -fp embc_mod_wrapper.c EmbC.build/mod_wrapper.c

EmbC.build/Kbuild: EmbC.build Kbuild.EmbC
	cp -fp Kbuild.EmbC EmbC.build/Kbuild

EmbC.build/EmbC.h: EmbC.build $(PWD)/../include/EmbC.h
	cp -fp $(PWD)/../include/EmbC.h EmbC.build/EmbC.h

EmbC.build/mod.ko: EmbC.build EmbC.build/mod_wrapper.c EmbC.build/mod_impl.c EmbC.build/EmbC.h EmbC.build/Kbuild EmbC.build/Makefile_for_Kbuild.EmbC
	make -C /usr/src/linux M=$(PWD)/EmbC.build EXTRA_INCL=$(PWD)/../include

EmbC.build:  
	mkdir -p EmbC.build

