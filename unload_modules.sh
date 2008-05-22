#!/bin/bash
PATH=${PATH}:/sbin:/usr/sbin:/usr/local/sbin

rmmod -w RealtimeFSM 2> /dev/null
rmmod -w LynxTrig_RT 2> /dev/null
rmmod -w LynxTWO_RT 2> /dev/null

rmmod ni_pcimio 2> /dev/null
rmmod ni_tio 2> /dev/null
rmmod comedi_fc 2> /dev/null
rmmod mite 2> /dev/null
rmmod 8255 2> /dev/null
rmmod zlib_deflate  2> /dev/null
rmmod kcomedilib  2> /dev/null
rmmod comedi 2> /dev/null
rmmod rtai_math 2> /dev/null
rmmod rtai_shm  2> /dev/null
rmmod rtai_sem 2> /dev/null
rmmod rtai_fifos  2> /dev/null
rmmod rtai_sched  2> /dev/null
rmmod rtai_hal 2> /dev/null

echo "All modules (hopefully) successfully unloaded.."





