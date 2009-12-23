#! /bin/bash

# A script that runs _s_prep.sh (unload modules that need to be discarded or refreshed), _s_start.sh (load third-party modules and RTFSM modules), and then starts up the servers.

cd /usr/src/rtfsm
sudo ./_s_prep.sh 
sudo ./_s_prep.sh 
sudo ./_s_prep.sh 
sudo ./_s_start.sh 
sudo ./SoundServer &
sudo ./FSMServer &

