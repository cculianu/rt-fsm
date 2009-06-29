#ifndef FSM_VERSION_H
#define FSM_VERSION_H

#define VersionSTR_Base "2009.06.28"
#define VersionNUM      220090628UL

#ifdef EMULATOR
#  define VersionSTR  VersionSTR_Base " EmbC.Emul"
#else
#  define VersionSTR  VersionSTR_Base " EmbC"
#endif


#endif
