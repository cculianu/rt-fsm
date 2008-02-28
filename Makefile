


all:  __KERNEL_MOD__ __USER_APP__ __USER_EXT_TRIG_MOD__ __SOUND_SERVER__ __UTILS__

__KERNEL_MOD__:
	$(MAKE) -C kernel && cp -f kernel/RealtimeFSM.ko .

__USER_APP__:
	$(MAKE) -C user && cp -f user/FSMServer .

__UTILS__:
	$(MAKE) -C utils

__USER_EXT_TRIG_MOD__:
	$(MAKE) -C addons/UserspaceExtTrig && cp -f addons/UserspaceExtTrig/UserspaceExtTrig.ko .
	
__SOUND_SERVER__:
	$(MAKE) -C addons/SoundTrig && cp -f addons/SoundTrig/SoundServer . && cp -f addons/SoundTrig/LynxTrig_RT.ko .
	
clean: 
	$(MAKE) -C kernel clean
	$(MAKE) -C user clean
	$(MAKE) -C utils clean
	$(MAKE) -C addons/UserspaceExtTrig/ clean
	$(MAKE) -C addons/SoundTrig/ clean
	rm -f RealtimeFSM.ko UserspaceExtTrig.ko FSMServer SoundServer LynxTrig_RT.ko *~ include/*~ both/*~
	find . -type f -name \*~ -exec rm -f {} \;
