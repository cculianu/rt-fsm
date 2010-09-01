#!/bin/bash

# <~> Clear off the crap that automatically starts so we can load the new RT system. There are some repeats. It doesn't matter. Run this script a few times.

/sbin/rmmod rtl_fifo rtl_posixio rtl_time comedi_bond ni_pcimio mite 8255 comedi_fc rtl_sched comedi


/sbin/rmmod kcomedilib
/sbin/rmmod rtai_sem
/sbin/rmmod rtai_shm
/sbin/rmmod rtai_fifos
/sbin/rmmod ni_pcimio
/sbin/rmmod rtai_sched
/sbin/rmmod comedi
/sbin/rmmod 8255
/sbin/rmmod ni_tio
/sbin/rmmod comedi_fc
/sbin/rmmod mite
/sbin/rmmod comedi
/sbin/rmmod rtai_hal

