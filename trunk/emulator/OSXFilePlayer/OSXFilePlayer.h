#ifndef OSXFilePlayer_H
#define OSXFilePlayer_H

#include "CAXException.h"

class OSXFilePlayer
{
public:
    OSXFilePlayer();
    ~OSXFilePlayer();


    /// use a loopct of 0xffffffff for infinite loops, or 1 for 1 play
    void setFile(const char *name, unsigned loopct = 1) throw (const CAXException &);
    /// after calling setfile, you can call duration to get the file duration in secs
    double duration() const;
    /// returns the numbre of loops specified in setfile
    unsigned loopCount() const;
    /// returns the filename previously specified in setfile
    const char *fileName() const;

    void play() throw (const CAXException &);
    void stop() throw (const CAXException &);

private:
    struct Impl;
    Impl *p;
};
#endif
