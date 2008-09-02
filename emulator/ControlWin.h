#ifndef ControlWin_H
#define ControlWin_H

#include <QWidget>
#include <map>
#include <QTime>
class QLabel;

class ControlWin : public QWidget
{
    Q_OBJECT
public:
    ControlWin(QWidget *parent = 0, Qt::WindowFlags f = 0);
public slots:
    void triggeredSound(unsigned id);
    void untriggeredSound(unsigned id);
    void resetAllSoundStats();
private slots:
    /// updates misc stats
    void updateMiscStats();     
private:
    QLabel *pausedValidRdyLbl, *cycleCtLbl, *tsLbl, *currentStateLbl, *transitionCountLbl, *schedWavesLbl, *aoWavesLbl, *sndsLbl;
    typedef std::map<unsigned, QTime> SndTimeMap;
    SndTimeMap lastPlayedSounds; ///< map of sndid -> expiry
};


#endif
