


all:  __KERNEL_MOD__ __USER_APP__ __UTILS__

__KERNEL_MOD__:
	$(MAKE) -C kernel && cp -f kernel/RealtimeFSM.ko .

__USER_APP__:
	$(MAKE) -C user && cp -f user/FSMServer .

__UTILS__:
	$(MAKE) -C utils

clean: 
	$(MAKE) -C kernel clean
	$(MAKE) -C user clean
	$(MAKE) -C utils clean
	rm -f RealtimeFSM.ko FSMServer *~ include/*~ both/*~
