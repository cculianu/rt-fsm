#!/bin/bash
PATH=${PATH}:/sbin:/usr/sbin:/usr/local/sbin
modprobe rtai_sched && \
modprobe rtai_fifos && \
modprobe rtai_sem && \
modprobe rtai_shm && \
modprobe rtai_math && \
modprobe kcomedilib && \
modprobe zlib_deflate 


if lspci | grep -i "National Instruments" | grep -iq "Class ff00"; then
    echo "National Instruments board detected.. loading driver and configuring /dev/comedi0"
    modprobe ni_pcimio && \
    comedi_config /dev/comedi0 ni_pcimio
else
    echo "Unknown or no DAQ board detected."
    echo "Setup comedi yourself, then load RealtimeFSM.ko, etc!"
    exit 0
fi

echo -n "Load RealtimeFSM.ko now [y/N]? "
read ans
if [ "$ans" == "Y" -o "$ans" == "y" ]; then
    insmod ./RealtimeFSM.ko
fi
echo -n "Load LynxTWO_RT.ko and LynxTrig_RT.ko now [y/N]? "
read ans
if [ "$ans" == "Y" -o "$ans" == "y" ]; then
    insmod ./LynxTWO_RT.ko
    insmod ./LynxTrig_RT.ko
else
    echo -n "Load UserspaceExtTrig.ko now [y/N]? "
    read ans
    if [ "$ans" == "Y" -o "$ans" == "y" ]; then
        insmod ./UserspaceExtTrig.ko
    fi
fi

echo "Ok, done.  Next start ./FSMServer (and optionally ./SoundServer)"




