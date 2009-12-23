#!/bin/bash

# A script that inserts 3rd party modules and then inserts RTFSM modules


#     Insert 3rd party modules
modprobe comedi
modprobe ni_pcimio
/usr/local/sbin/comedi_config /dev/comedi0 ni_pcimio
modprobe rtai_sched
modprobe rtai_fifos
modprobe rtai_math
modprobe rtai_shm
modprobe rtai_sem
modprobe kcomedilib
modprobe zlib_deflate


#     Insert module for national instruments card and configure comedi device.
sudo modprobe ni_pcimio
sudo /usr/local/sbin/comedi_config /dev/comedi0 ni_pcimio



#     Insert rtfsm module w/ cycle rate set to 1KHz.
cd /usr/src/rtfsm
insmod ./RealtimeFSM.ko task_rate=2000

#     Insert modules necessary for hard realtime sound server.
insmod ./LynxTWO_RT.ko
insmod ./LynxTrig_RT.ko


#     Starting the servers:
# ./FSMServer &
# ./SoundServer &

