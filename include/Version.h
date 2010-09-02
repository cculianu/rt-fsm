#ifndef FSM_VERSION_H
#define FSM_VERSION_H

#define VersionSTR_Base "2010.09.02"
#define VersionNUM      220100902UL

#ifdef EMULATOR
#  define VersionSTR  VersionSTR_Base " EmbC.Emul"
#else
#  define VersionSTR  VersionSTR_Base " EmbC"
#endif


#endif
