#include "SoundPlayer.h"
#include "Log.h"

#ifdef Q_OS_DARWIN 
/* NB: we need to use CoreAudio on OSX because sound playing using QSound on 
   darwin is BROKEN!! */
#include "OSXFilePlayer/OSXFilePlayer.h"

struct SoundPlayer::Impl : public OSXFilePlayer
{
};

SoundPlayer::SoundPlayer(const QString & fname,
                         QObject * parent,
                         bool loops)
    : QObject(parent)
{
    p = new Impl;
    p->setFile(fname.toUtf8().constData(), loops ? 0xffffffff : 1);
}

QString SoundPlayer::fileName() const { return p->fileName(); }
void SoundPlayer::play() 
{ 
    try {
        p->play(); 
    } catch (const CAXException &e) {
        Error() << "ERROR playing using " << fileName() << "; error from OSXFilePlayer was: (" << e.mError << ") '" << e.mOperation << "'";
    }
}
void SoundPlayer::stop() { try { p->stop(); } catch (...) {} }
bool SoundPlayer::loops() const { return p->loopCount() > 1; }

#else // !DARWIN

#include <QSound>

struct SoundPlayer::Impl
{
    Impl(const QString & fname) : s(fname), fname(fname) {}    
    QSound s;
    QString fname;
    bool loops;
};

SoundPlayer::SoundPlayer(const QString & fname,
                         QObject * parent,
                         bool loops)
    : QObject(parent)
{
    p = new Impl(fname);
    p->loops = loops;
    p->s.setLoops(loops ? -1 : 1);
}

QString SoundPlayer::fileName() const { return p->s.fileName(); }
void SoundPlayer::play() { p->s.setLoops(p->loops ? -1 : 1); p->s.play(); }
void SoundPlayer::stop() { p->s.stop(); }
bool SoundPlayer::loops() const { return p->loops; }

#endif


SoundPlayer::~SoundPlayer()
{
    delete p;
    p = 0;
}
