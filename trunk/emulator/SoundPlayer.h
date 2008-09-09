#ifndef SoundPlayer_H
#define SoundPlayer_H
#include <QString>
#include <QObject>

/// Abstracts out the soundplayer since on OSX we use Phonon and on
/// all other platforms we use QSound.
class SoundPlayer : public QObject
{
#ifdef Q_OS_DARWIN
    Q_OBJECT
#endif
public:
    SoundPlayer(const QString &filename, QObject *parent = 0, bool loops = false);
    ~SoundPlayer();
    QString fileName() const;
    void play();
    void stop();
    void setLoops(bool loops);
    bool loops() const;
#ifdef Q_OS_DARWIN
protected slots:
    void enqueueNextSource();
#endif
private:
    struct Impl;
    Impl *p;
};

#endif
