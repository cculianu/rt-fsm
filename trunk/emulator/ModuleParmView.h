#ifndef ModuleParmView_H
#define ModuleParmView_H

#include <QWidget>
#include <map>
#include <QString>
#include "kernel_emul.h"

class QLineEdit;
class QGridLayout;
class QValidator;

class ModuleParmView : public QWidget
{
    Q_OBJECT

public:
    ModuleParmView(QWidget *parent = 0, Qt::WindowFlags f = 0);
    /// change which map we are viewing
    void setModuleParms(const Emul::ModuleParmMap *);
    /// saves all the values in the QLineEdits to the module parm map
    void applyChanges();
    /// reverts all the values 
    void revertChanges();

signals:
    void changed();

private:
    void clear();

    typedef std::map<QString, QLineEdit *> ParmLineEditMap;
    const Emul::ModuleParmMap *mpm;
    ParmLineEditMap plem;
    QGridLayout *lo;
    QValidator *intValidator;
};

#endif
