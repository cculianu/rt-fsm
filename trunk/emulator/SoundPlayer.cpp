#include "SoundPlayer.h"

#ifdef Q_OS_DARWIN 
/* NB: we need to use phonon on OSX because sound playing using QSound on 
   darwin is BROKEN!! */
#include <phonon>

struct SoundPlayer::Impl
{
    Phonon::MediaObject *s;
    QString fname;
    bool loops;
    ~Impl() { s->stop(); delete s; }
};

SoundPlayer::SoundPlayer(const QString & fname,
                         QObject * parent,
                         bool loops)
    : QObject(parent)
{
    p = new Impl;
    p->s = Phonon::createPlayer(Phonon::MusicCategory, Phonon::MediaSource(fname));
    p->fname = fname;
    p->loops = loops;
    connect(p->s, SIGNAL(currentSourceChanged(const Phonon::MediaSource &)), this, SLOT(enqueueNextSource()));
}

QString SoundPlayer::fileName() const { return p->fname; }
void SoundPlayer::play() 
{ 
    p->s->clearQueue();
    // re-enqueue the sound source for infinite looping
    p->s->setCurrentSource(p->fname);
    enqueueNextSource();
    p->s->play(); 
}
void SoundPlayer::stop() { p->s->stop(); }
void SoundPlayer::setLoops(bool loops) { p->loops = loops; }
bool SoundPlayer::loops() const { return p->loops; }

void SoundPlayer::enqueueNextSource()
{
    if (p->loops) {
        // re-enqueue the sound source for infinite looping
        p->s->enqueue(p->fname);
    }
}

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
void SoundPlayer::play() { setLoops(p->loops); p->s.play(); }
void SoundPlayer::stop() { p->s.stop(); }
void SoundPlayer::setLoops(bool loops) { p->loops = loops; p->s.setLoops(loops ? -1 : 1); }
bool SoundPlayer::loops() const { return p->loops; }

#endif


SoundPlayer::~SoundPlayer()
{
    delete p;
    p = 0;
}
