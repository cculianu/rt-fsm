#!/bin/bash

# <~> Clear off the crap that automatically starts so we can load the new RT system. There are some repeats. It doesn't matter. Run this script a few times.

rmmod rtl_fifo rtl_posixio rtl_time comedi_bond ni_pcimio mite 8255 comedi_fc rtl_sched comedi


rmmod kcomedilib
rmmod rtai_sem
rmmod rtai_shm
rmmod rtai_fifos
rmmod ni_pcimio
rmmod rtai_sched
rmmod comedi
rmmod 8255
rmmod ni_tio
rmmod comedi_fc
rmmod mite
rmmod comedi
rmmod rtai_hal
