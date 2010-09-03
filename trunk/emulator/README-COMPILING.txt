----------------------------------------------------------------------------------------------------
------ 2010-09-02 Notes by Praveen Karri, computer analyst in the Brody lab, on compiling the emulator on Windows: ----- 


							............ To just compile the emulator and use it in the same machine read below ...............

Installing MinGW on Windows:

Download MinGW automated installer from here -> http://sourceforge.net/projects/mingw/files/Automated%20MinGW%20Installer/mingw-get/mingw-get-0.1-alpha-3/mingw-get-0.1-mingw32-alpha-3-bin.zip/download
Unzip it into (say) D:\MinGW
Goto command prompt and change directory to D:\MinGW\bin
Type the following command -> mingw-get install gcc g++ mingw32-make gdb msys-base objc

MinGW is now installed.

Installing Qt on Windows:

Download and install (say in D:\Qt) -> http://get.qt.nokia.com/qtsdk/qt-sdk-win-opensource-2010.04.exe

The following is a summary of the changes made and the complete procedure to compile (both in debug and release modes) and run FSMEmulator on Windows:

-> Assuming the following are paths of rt-fsm, Qt installation folder and MinGW installation folder, 

Qt installed in D:\Qt
MinGW installed in D:\MinGW
rt-fsm resides in D:\rt-fsm

the following has to be done first:

Find user variables and system variables in 'System Properties -> Advanced -> Environment Variables' in any Windows Machine by right-clicking on 'My Computer' and going to 'Properties'.
Add the following path 'D:\MinGW\bin;' to %PATH% in user variables. 
Add the following paths 'D:\Qt\2010.04\qt\bin;' and D:\MinGW\bin;' to %PATH% in system variables.

Restart Windows now.

Compile the emulator using the following:

D:\rt-fsm\emulator\compile-win32.bat -> For Debug mode output
D:\rt-fsm\emulator\compile-win32-release.bat -> For Release mode output

If compiling in release mode, copy the following files into 'D:\rt-fsm\emulator\win32' folder

D:\Qt\2010.04\qt\bin\QtCore4.dll
D:\Qt\2010.04\qt\bin\QtGui4.dll

Now, FSMEmulator.exe should run fine.

-> Additional Info :

The above two Qt dll files are often confused with the following files (with same name) which are present in a similar Qt installation directory as shown below:

D:\Qt\2010.04\bin\QtCore4.dll
D:\Qt\2010.04\bin\QtGui4.dll

Do not use these files.

The reason why we have to copy the two dll files when compiling in release mode is because, during the release mode compilation, Qt uses wrong paths to create the final executable (a problem which should be fixed by people who developed Qt and not us).

-> Changes made: (note-- the three following changes have now been made in the Google reporsitory, as of revision 190, so if you're at revision 190 or above, you don't need to worry about them):

D:\rt-fsm\emulator\win32\pthread.dll is deleted. 
D:\rt-fsm\emulator\win32\pthreadGCE2.dll renamed to D:\rt-fsm\emulator\win32\pthread.dll
D:\rt-fsm\include\libz.dll copied to D:\rt-fsm\emulator\win32\libz.dll

Note: Compiling in release mode is recommended because it is more mobile and can be used on other windows machines without the worry of dependencies.

----------------------------------------------------------------------------------------------------

						............ To make the emulator distributable after compilation read below ...............

Irrespective of compilation mode, to make the emulator distributable, copy the following four files into 'D:\rt-fsm\emulator\win32' folder

D:\Qt\2010.04\qt\bin\QtCore4.dll
D:\Qt\2010.04\qt\bin\QtGui4.dll
D:\Qt\2010.04\qt\bin\libgcc_s_dw2-1.dll
D:\Qt\2010.04\qt\bin\mingwm10.dll
D:\MinGW\bin\libstdc++-6.dll

Note: To use the emulator on Windows Vista or 7, run it in WinXP compatibility mode.

--------------------------------------------------------------------------------------------------

