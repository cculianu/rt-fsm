#ifndef SoundPlayer_H
#define SoundPlayer_H
#include <QString>
#include <QObject>

/// Abstracts out the soundplayer since on OSX we use Phonon and on
/// all other platforms we use QSound.
class SoundPlayer : public QObject
{
public:
    SoundPlayer(const QString &filename, QObject *parent = 0, bool loops = false);
    ~SoundPlayer();
    QString fileName() const;
    bool loops() const;
    void play();
    void stop();
private:
    struct Impl;
    Impl *p;
};

#endif
