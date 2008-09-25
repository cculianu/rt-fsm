set QMAKESPEC=win32-g++
qmake
mingw32-make 
copy /Y debug\FSMEmulator.exe win32
g++ -g -mno-cygwin -DWINDOWS -DWIN32 -DEMULATOR -DOS_WINDOWS -o win32\FSMServer.exe ..\user\FSMServer.cpp ..\user\rtos_utility.cpp ..\user\Util.cpp ..\kernel\deflate_helper.c -I ..\user -I ..\kernel -I ..\include -I .\win32 -lws2_32 -L.\win32 -lpthread -lz -lregex
g++ -g -mno-cygwin  -DWINDOWS -DWIN32 -DEMULATOR -DOS_WINDOWS -o win32\SoundServer.exe ..\addons\SoundTrig\SoundServer.cpp ..\addons\SoundTrig\WavFile.cpp ..\addons\SoundTrig\libresample.c ..\user\rtos_utility.cpp ..\user\Util.cpp -I ..\user -I ..\addons\UserspaceExtTrig -I ..\include -I .\win32 -l ws2_32 -L .\win32 -lpthread -lwinmm
