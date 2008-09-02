/***************************************************************************
                          scanproc.h  -  Utility functions for /proc scanning
                             -------------------
    begin                : Thu Feb 7 2002
    copyright            : (C) 2002 by Calin Culianu
    email                : calin@rtlab.org
 ***************************************************************************/
 
/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/
#ifndef SCANPROCS_H
#define SCANPROCS_H 1

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

  /* 
     For next two functions...

     returns array of pid_t's representing all the processes that are running
     a certain exe -- requires /proc support.  The array is terminated by
     a pit_t == 0, and is allocated using malloc() 
     
     returns NULL on error...
  */
  extern pid_t *pids_of_exe(const char *exe_path);
  extern pid_t *pids_of_my_exe(void);


  extern int    num_procs_of_my_exe(void);
  /* like above but discounts children that may be the result of LWP/pthreads*/
  extern int    num_procs_of_my_exe_no_children(void);

  extern int    num_procs_of_exe(const char *exe_path);
  /* like above but discounts children that may be the result of LWP/pthreads*/
  extern int    num_procs_of_exe_no_children(const char *exe);
#ifndef OS_OSX
  /* 
     returns a malloc'd char * string (which might be NULL on error)
     if sz != NULL then it stores the string size in sz.

     opens /proc/pid/cmdline and figures out the bare name 
     (last path component)  of the first argv[] in the contents of the file 
     /proc/PID/cmdline (if any)

     Any errors return NULL
  */
  extern char * grab_stripped_cmd_name_of_pid(pid_t pid, int *sz);
  /* 
     returns a malloc'd char * string (which might be NULL on error)
     if sz != NULL then it stores the string size in sz.

     opens /proc/pid/cmdline and returns the full contents of
     the first argv[] in the contents of the file (if any)

     Any errors return NULL
  */
  extern char * grab_full_cmd_name_of_pid(pid_t pid, int *sz);

  /* same as above but wrappers using getpid() */
  extern char * grab_my_full_cmd_name(int *sz);
  extern char * grab_my_stripped_cmd_name(int *sz);
#endif
  /* scan /proc/PID/status and grab the PPid of a proces, if any */
  extern pid_t grab_parent_of_pid(pid_t pid);

#ifndef OS_OSX
  struct ModList
  {
    struct ModList *next;
    char *mod;
    size_t size;
    char unused_flg;
    char autoclean_flg;
    char **refs; /* NB: these are modules that depend ON this module! */
    int n_refs;
    int use_ct;
  };

  /* returns a struct ModList of all the modules listed in /proc/modules */
  extern const struct ModList * get_module_list(void);  
  /* use this function to freee a struct ModList created with module_list() */
  extern void free_module_list(const struct ModList *);
  extern const struct ModList * find_module_in_modlist(const struct ModList *,
                                                       const char *);
#endif
  
#ifdef __cplusplus
}
#endif

#endif
