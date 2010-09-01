#!/bin/bash

# A script that inserts 3rd party modules and then inserts RTFSM modules


#     Insert 3rd party modules
/sbin/modprobe comedi
/sbin/modprobe ni_pcimio
/usr/local/sbin/comedi_config /dev/comedi0 ni_pcimio
/sbin/modprobe rtai_sched
/sbin/modprobe rtai_fifos
/sbin/modprobe rtai_math
/sbin/modprobe rtai_shm
/sbin/modprobe rtai_sem
/sbin/modprobe kcomedilib
/sbin/modprobe zlib_deflate


#     Insert module for national instruments card and configure comedi device.
/sbin/modprobe ni_pcimio
/usr/local/sbin/comedi_config /dev/comedi0 ni_pcimio



#     Insert rtfsm module w/ cycle rate set to 1KHz.
cd /usr/src/rtfsm
/sbin/insmod ./RealtimeFSM.ko task_rate=2000

#     Insert modules necessary for hard realtime sound server.
/sbin/insmod ./LynxTWO_RT.ko
/sbin/insmod ./LynxTrig_RT.ko


#     Starting the servers:
# ./FSMServer &
# ./SoundServer &

