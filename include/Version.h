#ifndef FSM_VERSION_H
#define FSM_VERSION_H

#define VersionSTR_Base "2008.10.09"
#define VersionNUM      220081009UL

#ifdef EMULATOR
#  define VersionSTR  VersionSTR_Base " EmbC.Emul"
#else
#  define VersionSTR  VersionSTR_Base " EmbC"
#endif


#endif
