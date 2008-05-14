
#include <math.h>
#include <string>
#include <string.h>
#include <mex.h>
#include <matrix.h>

#include <map>

#include "NetClient.h"

typedef std::map<int, NetClient *> NetClientMap;

#ifndef WIN32
#define strcmpi strcasecmp
#endif

// url encode a string
static long UrlEncode(char *dest, const char *src);

static NetClientMap clientMap;
static int handleId = 0; // keeps getting incremented..

static NetClient * MapFind(int handle)
{
  NetClientMap::iterator it = clientMap.find(handle);
  if (it == clientMap.end()) return NULL;
  return it->second;
}

static void MapPut(int handle, NetClient *client)
{
  NetClient *old = MapFind(handle);
  if (old) delete old; // ergh.. this shouldn't happen but.. oh well.
  clientMap[handle] = client;
}

static void MapDestroy(int handle)
{
  NetClientMap::iterator it = clientMap.find(handle);
  if (it != clientMap.end()) {
    delete it->second;  
    clientMap.erase(it);
  } else {
    mexWarnMsgTxt("Invalid or unknown handle passed to FSMClient MapDestroy!");
  }
}

static int GetHandle(int nrhs, const mxArray *prhs[])
{
  if (nrhs < 1) 
    mexErrMsgTxt("Need numeric handle argument!");

  const mxArray *handle = prhs[0];

  if ( !mxIsDouble(handle) || mxGetM(handle) != 1 || mxGetN(handle) != 1) 
    mexErrMsgTxt("Handle must be a single double value.");

  return static_cast<int>(*mxGetPr(handle));
}

static NetClient * GetNetClient(int nrhs, const mxArray *prhs[])
{
  int handle =  GetHandle(nrhs, prhs);
  NetClient *nc = MapFind(handle);
  if (!nc) mexErrMsgTxt("INTERNAL ERROR -- Cannot find the NetClient for the specified handle in FSMClient!");
  return nc;
}

#define RETURN(x) do { (plhs[0] = mxCreateDoubleScalar(static_cast<double>(x))); return; } while (0)
#define RETURN_NULL() do { (plhs[0] = mxCreateDoubleMatrix(0, 0, mxREAL)); return; } while(0)

void createNewClient(int nlhs, mxArray *plhs[], int nrhs, const mxArray *prhs[])
{
  if (nlhs != 1) mexErrMsgTxt("Cannot create a client since no output (lhs) arguments were specified!");
  if (nrhs != 2) mexErrMsgTxt("Need two input arguments: Host, port!");
  const mxArray *host = prhs[0], *port = prhs[1];
  if ( !mxIsChar(host) || mxGetM(host) != 1 ) mexErrMsgTxt("Hostname must be a string row vector!");
  if ( !mxIsDouble(port) || mxGetM(port) != 1 || mxGetN(port) != 1) mexErrMsgTxt("Port must be a single numeric value.");
  
  char *hostStr = mxArrayToString(host);
  unsigned short portNum = static_cast<unsigned short>(*mxGetPr(port));
  NetClient *nc = new NetClient(hostStr, portNum);
  mxFree(hostStr);
  nc->setSocketOption(Socket::TCPNoDelay, true);
  int h = handleId++;
  MapPut(h, nc);
  RETURN(h);
}

void tryConnection(int nlhs, mxArray *plhs[], int nrhs, const mxArray *prhs[])
{
  NetClient *nc = GetNetClient(nrhs, prhs);
  //if(nlhs < 1) mexErrMsgTxt("One output argument required.");
  bool ok = false;
  try {
    ok = nc->connect();
  } catch (const SocketException & e) {
    mexWarnMsgTxt(e.why().c_str());
    RETURN_NULL();
  }

  if (!ok) {
    mexWarnMsgTxt(nc->errorReason().c_str());
    RETURN_NULL();
  }

  RETURN(1);
}
 
void closeSocket(int nlhs, mxArray *plhs[], int nrhs, const mxArray *prhs[])
{
  NetClient *nc = GetNetClient(nrhs, prhs);
  nc->disconnect();
  RETURN(1);
}


void sendString(int nlhs, mxArray *plhs[], int nrhs, const mxArray *prhs[])
{
  NetClient *nc = GetNetClient(nrhs, prhs);
  
  if(nrhs != 2)		mexErrMsgTxt("Two arguments required: handle, string.");
  //if(nlhs < 1) mexErrMsgTxt("One output argument required.");
  if(mxGetClassID(prhs[1]) != mxCHAR_CLASS) 
	  mexErrMsgTxt("Argument 2 must be a string.");

  char *tmp = mxArrayToString(prhs[1]);
  std::string theString (tmp);
  mxFree(tmp);

  try {
    nc->sendString(theString);
  } catch (const SocketException & e) {
    mexWarnMsgTxt(e.why().c_str());
    RETURN_NULL();
  }  
  RETURN(1);
}

void sendMatrix(int nlhs, mxArray *plhs[], int nrhs, const mxArray *prhs[])
{
  NetClient *nc = GetNetClient(nrhs, prhs);

  if(nrhs != 2)		mexErrMsgTxt("Two arguments required: handle, matrix.");
  //if(nlhs < 1) mexErrMsgTxt("One output argument required.");
  if(mxGetClassID(prhs[0]) != mxDOUBLE_CLASS) 
	  mexErrMsgTxt("Argument 2 must be a matrix of doubles.");

  double *theMatrix = mxGetPr(prhs[1]);
  int msglen = mxGetN(prhs[1]) * mxGetM(prhs[1]) * sizeof(double);

  try {
    nc->sendData(theMatrix, msglen);
  } catch (const SocketException & e) {
    mexWarnMsgTxt(e.why().c_str());
    RETURN_NULL();
  }
  
  RETURN(1);
}

void readString(int nlhs, mxArray *plhs[], int nrhs, const mxArray *prhs[])
{
  if(nlhs < 1) mexErrMsgTxt("One output argument required.");
  NetClient *nc = GetNetClient(nrhs, prhs);

  try {
    std::string theString ( nc->receiveString() );
    plhs[0] = mxCreateString(theString.c_str());
  } catch (const SocketException & e) {
    mexWarnMsgTxt(e.why().c_str());
    RETURN_NULL(); // note empty return..
  }
}

void readLines(int nlhs, mxArray *plhs[], int nrhs, const mxArray *prhs[])
{
  if(nlhs < 1) mexErrMsgTxt("One output argument required.");

  NetClient *nc = GetNetClient(nrhs, prhs);
  try {
    char **lines = nc->receiveLines();
    int m;
    for (m = 0; lines[m]; m++) {} // count number of lines
    plhs[0] = mxCreateCharMatrixFromStrings(m, const_cast<const char **>(lines));
    NetClient::deleteReceivedLines(lines);
  } catch (const SocketException &e) {
    mexWarnMsgTxt(e.why().c_str());
    RETURN_NULL(); // empty return set
  }
}


void readMatrix(int nlhs, mxArray *plhs[], int nrhs, const mxArray *prhs[])
{
  NetClient *nc = GetNetClient(nrhs, prhs);
  if(nrhs != 3 || !mxIsDouble(prhs[1]) || !mxIsDouble(prhs[2]) ) mexErrMsgTxt("Output matrix size M,N as 2 real arguments are required.");
  if(nlhs < 1) mexErrMsgTxt("One output argument required.");
  
  int m = static_cast<int>(*mxGetPr(prhs[1])),
	  n = static_cast<int>(*mxGetPr(prhs[2]));
  
  int dataLen = m*n*sizeof(double);
  
  
  plhs[0] = mxCreateDoubleMatrix(m, n, mxREAL);
  
  try {
    nc->receiveData(mxGetPr(plhs[0]), dataLen, true);
  } catch (const SocketException & e) {
    mexWarnMsgTxt(e.why().c_str());
    mxDestroyArray(plhs[0]);
    plhs[0] = 0;  // nullify (empty) return..
    RETURN_NULL();
  }
}  

#ifdef WIN32
#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <iostream>

namespace {
  namespace Notify {
    bool haveProc = false, didAtExit = false;
    std::string cb, cbconnfail; 
    int handle;
    PROCESS_INFORMATION proc;
    STARTUPINFO si;

    void cleanup(void) 
    {
      if (Notify::haveProc) {
        TerminateProcess(Notify::proc.hProcess, -1);
        CloseHandle(Notify::proc.hProcess);
        CloseHandle(Notify::proc.hThread);
        Notify::haveProc = false;
      }
    }    
  };

  void notifyEvents(int nlhs, mxArray *plhs[], int nrhs, const mxArray *prhs[])
  {
    if (nrhs < 2) mexErrMsgTxt("Please pass: handle, callback.");
    int h = GetHandle(nrhs, prhs);
    NetClient *nc = GetNetClient(nrhs, prhs);
    if (!mxIsChar(prhs[1])) mexErrMsgTxt("Callback must be a string.");
    char *tmp = mxArrayToString(prhs[1]);
    Notify::cb = tmp;
    mxFree(tmp);    
    if (nrhs > 2 && mxIsChar(prhs[2])) {
      tmp = mxArrayToString(prhs[2]);
      Notify::cbconnfail = tmp;
      mxFree(tmp);    
    } else {
      Notify::cbconnfail = "";
    }
    Notify::cleanup(); // kills any notify process that might be running
    char arg1[512];
    sprintf(arg1, "%s:%hu", nc->host().c_str(), nc->port());
    arg1[512] = 0;
    const char * cmd = "FSM_Event_Notification_Helper_Process.exe";
    std::string cmdline = cmd;    
    cmdline += std::string(" ") + arg1 + " \"" + Notify::cb + "\"";
    if (!Notify::cbconnfail.empty())
      cmdline += " \"" + Notify::cbconnfail + "\"";
    memset(&Notify::si, 0, sizeof(Notify::si));
    memset(&Notify::proc, 0, sizeof(Notify::proc));
    if (!CreateProcessA(cmd, const_cast<char *>(cmdline.c_str()), 0, 0, false, NORMAL_PRIORITY_CLASS, 0, 0, &Notify::si, &Notify::proc)) {
      if (GetLastError() == ERROR_FILE_NOT_FOUND) {
        sprintf(arg1, "FSMClient could not launch %s.  Are you sure it's in your PATH?", cmd);
      } else {
        sprintf(arg1, "FSMClient could not launch %s:  CreateProcess returned %d.",cmd, GetLastError());
      }
      arg1[127] = 0;
      mexErrMsgTxt(arg1);
    }    
    Notify::handle = h;
    Notify::haveProc = true;
    if (!Notify::didAtExit) {
      ::atexit(&Notify::cleanup);
      Notify::didAtExit = true;
    }
    RETURN(1);
  }

  void stopNotifyEvents(int nlhs, mxArray *plhs[], int nrhs, const mxArray *prhs[])
  {
    Notify::cleanup();
    RETURN(1);
  }
  
};
#else
  void notifyEvents(int nlhs, mxArray *plhs[], int nrhs, const mxArray *prhs[])
  {
    mexErrMsgTxt("notifyEvents in FSMClient only implemented for Matlab on Windows, sorry.");    
  }
  void stopNotifyEvents(int nlhs, mxArray *plhs[], int nrhs, const mxArray *prhs[])
{
    mexErrMsgTxt("stopNotifyEvents in FSMClient only implemented for Matlab on Windows, sorry.");    
  }
#endif

void destroyClient(int nlhs, mxArray *plhs[], int nrhs, const mxArray *prhs[])
{
  int h = GetHandle(nrhs, prhs);
  MapDestroy(h);
#ifdef WIN32
  // kill running proxy notify process
  if (h == Notify::handle)  Notify::cleanup();
#endif
  RETURN(1);
}

void formatMatrix(int nlhs, mxArray *plhs[], int nrhs, const mxArray *prhs[])
{
/*    if (nlhs < 1) {
        mexWarnMsgTxt("formatMatrix did not get an output argument, so not doing any real work.  Specify an output argument to do real work.");
        RETURN_NULL();
    }*/
    if (nrhs != 1 || !mxIsCell(*prhs) || !mxGetNumberOfDimensions(*prhs)) 
        mexErrMsgTxt("formatMatrix: needs one input argument of type cell");
    const int m = mxGetM(*prhs), n = mxGetN(*prhs);
    int i,j;
    static long outbuf_len = 1024*1024; // preallocate 1MB
    char *outbuf = (char *)mxCalloc(outbuf_len, sizeof(char)), *outbufPtr = outbuf; // preallocate 1MB for output
    if (!outbuf)
        mexErrMsgTxt("formatMatrix: failed to allocate 1MB for output txt");
    for (i = 0; i < m; ++i) {
        for (j = 0; j < n; ++j) {
            int subs[2] = { i, j };
            char buf[4096];
            mxArray *cell = mxGetCell(*prhs, mxCalcSingleSubscript(*prhs, 2, subs));
            if (!cell) {
                mxFree(outbuf);
                mexErrMsgTxt("formatMatrix: A cell element in the cell array could not be retrieved!");
            }
            void *data = mxGetData(cell);
            if (!data) { // can happen on empty array
                mxFree(outbuf);
                mexErrMsgTxt("formatMatrix: A cell element in the cell array is empty/null -- all elements need to contain data!");
            }
            switch(mxGetClassID(cell)) {
                case mxCHAR_CLASS: 
                    mxGetString(cell, buf, sizeof(buf));
                    buf[sizeof(buf)-1] = 0;
                    break;
                case mxINT8_CLASS:   sprintf(buf, "%hhd", *(char *)mxGetData(cell)); break; 
                case mxUINT8_CLASS:  sprintf(buf, "%hhu", *(unsigned char *)mxGetData(cell)); break; 
                case mxINT16_CLASS:  sprintf(buf, "%hd", *(short *)mxGetData(cell)); break; 
                case mxUINT16_CLASS: sprintf(buf, "%hu", *(unsigned short *)mxGetData(cell)); break; 
                case mxINT32_CLASS:  sprintf(buf, "%d", *(int *)mxGetData(cell)); break; 
                case mxUINT32_CLASS: sprintf(buf, "%u", *(unsigned int *)mxGetData(cell)); break; 
                case mxSINGLE_CLASS: sprintf(buf, "%g", *(float *)mxGetData(cell)); break; 
                case mxDOUBLE_CLASS: sprintf(buf, "%g", *(double *)mxGetData(cell)); break; 
                default:
                    mxFree(outbuf);
                    mexErrMsgTxt("formatMatrix: Unknown cell type encountered!");
            }
            long lenSoFar = outbufPtr - outbuf;
            if (lenSoFar + (strlen(buf)*3+4) >= outbuf_len) {
                // reallocate if ran out of space
                void * tmp = mxRealloc(outbuf, outbuf_len*=2);
                if (!tmp) {
                    mxFree(outbuf);
                    mexErrMsgTxt("formatMatrix: Out of space in output text buffer and failed to allocate more!");
                }
                outbuf = (char *)tmp;
                outbufPtr = outbuf + lenSoFar;
            }
            // it's 1 cell per line....
            *outbufPtr++ = ' ';
            *outbufPtr++ = ' ';
            outbufPtr += UrlEncode(outbufPtr, buf);
            *outbufPtr++ = '\n';
        }
    }
   // return the text..
   if (plhs[0])
       mxDestroyArray(plhs[0]);
   plhs[0] = mxCreateString(outbuf);
   mxFree(outbuf);
}

struct CommandFunction
{
	const char *name;
	void (*func)(int, mxArray **, int, const mxArray **);
};

static struct CommandFunction functions[] = 
{
    { "create", createNewClient },
    { "destroy", destroyClient },
	{ "connect", tryConnection },
	{ "disconnect", closeSocket },
	{ "sendString", sendString },
	{ "sendMatrix", sendMatrix },
	{ "readString", readString },
	{ "readLines",  readLines},
	{ "readMatrix", readMatrix },
    { "notifyEvents", notifyEvents },
    { "stopNotifyEvents", stopNotifyEvents },
    { "formatMatrix", formatMatrix }
};

static const int n_functions = sizeof(functions)/sizeof(struct CommandFunction);

void mexFunction( int nlhs, mxArray *plhs[],
                  int nrhs, const mxArray *prhs[])
{
  const mxArray *cmd;
  int i;
  std::string cmdname;

  /* Check for proper number of arguments. */
  if(nrhs < 2) 
    mexErrMsgTxt("At least two input arguments are required.");
  else 
    cmd = prhs[0];
  
  if(!mxIsChar(cmd)) mexErrMsgTxt("COMMAND argument must be a string.");
  if(mxGetM(cmd) != 1)   mexErrMsgTxt("COMMAND argument must be a row vector.");
  char *tmp = mxArrayToString(cmd);
  cmdname = tmp;
  mxFree(tmp);
  for (i = 0; i < n_functions; ++i) {
	  // try and match cmdname to a command we know about
    if (::strcmpi(functions[i].name, cmdname.c_str()) == 0 ) { 
         // a match.., call function for the command, popping off first prhs
		functions[i].func(nlhs, plhs, nrhs-1, prhs+1); // call function by function pointer...
		break;
	  }
  }
  if (i == n_functions) { // cmdname didn't match anything we know about
	  std::string errString = "Unrecognized FSMClient command.  Must be one of: ";
	  for (int i = 0; i < n_functions; ++i)
        errString += std::string(i ? ", " : "") + functions[i].name;
	  mexErrMsgTxt(errString.c_str());
  }
}

/* Encode funny chars -> %xx in newly allocated storage */
/* (preserves '/' !) */
/* Make sure t holds at least strlen(s)*3+1 -Doug */
/* Returns the size of the generated output string */
long UrlEncode(char *t, const char *s) 
{
    unsigned char *tp = (unsigned char *)t;
    const unsigned char *p = (const unsigned char *)s;
    if (!t) return 0;
    for (p = (const unsigned char *)s; *p; p++) {
        if( (*p > 0x00 && *p < ',') ||
            (*p > '9' && *p < 'A') ||
            (*p > 'Z' && *p < '_') ||
            (*p > '_' && *p < 'a') ||
            (*p > 'z' && *p < 0xA1)) {
            sprintf((char *)tp, "%%%02X", (char)*p);
            tp += 3;
        } else {
            *tp = *p;
            tp++;
        }
    }
    *tp = 0;
    return tp - (unsigned char *)t;
}
