#include "ModuleParmView.h"
#include <QLineEdit>
#include <QLabel>
#include <QGridLayout>
#include <QList>
#include <QIntValidator>

ModuleParmView::ModuleParmView(QWidget *parent, Qt::WindowFlags f)
    : QWidget(parent, f), mpm(0)
{
    lo = new QGridLayout(this);
    intValidator = new QIntValidator(this);
}

void ModuleParmView::clear()
{
    QList<QObject *> chld = children();
    for (QList<QObject *>::iterator it = chld.begin(); it != chld.end(); ++it) {
        if (dynamic_cast<QLineEdit *>(*it) || dynamic_cast<QLabel *>(*it))
            delete (*it);
    }
    plem.clear();
    mpm = 0;
}

void ModuleParmView::setModuleParms(const Emul::ModuleParmMap *m_in)
{   
    clear();    
    mpm = m_in;
    if (!mpm) return;
    const Emul::ModuleParmMap & m = *mpm;
    int r = 0;
    for (Emul::ModuleParmMap::const_iterator it = m.begin(); it != m.end(); ++it, ++r) {
        QString pname (it->first.c_str());
        const Emul::ModuleParm & p = it->second;
        if (p.type != Emul::ModuleParm::Int) continue;
        const int & i = *reinterpret_cast<const int *>(p.p);
        QLabel *l = new QLabel(pname, this);
        QLineEdit *e = new QLineEdit(QString::number(i), this);
        e->setToolTip(p.desc.c_str());
        e->setWhatsThis(p.desc.c_str());
        //e->setStatusTip(p.desc.c_str());
        l->setToolTip(p.desc.c_str());
        l->setWhatsThis(p.desc.c_str());
        //l->setStatusTip(p.desc.c_str());  
        connect(e, SIGNAL(textEdited(const QString &)), this, SIGNAL(changed()));
        e->setValidator(intValidator);
        lo->addWidget(l, r, 0, Qt::AlignRight);
        lo->addWidget(e, r, 1);
        plem[pname] = e;
    }
}

/// reverts all the values 
void ModuleParmView::revertChanges()
{
    const Emul::ModuleParmMap & m = *mpm;
    for (Emul::ModuleParmMap::const_iterator it = m.begin(); it != m.end(); ++it) {
        QString pname (it->first.c_str());
        const Emul::ModuleParm & p = it->second;
        if (p.type != Emul::ModuleParm::Int) continue;
        const int & i = *reinterpret_cast<const int *>(p.p);
        ParmLineEditMap::iterator pit = plem.find(pname);
        if (pit == plem.end()) continue;
        pit->second->setText(QString::number(i));
    }
}

/// reverts all the values 
void ModuleParmView::applyChanges()
{
    const Emul::ModuleParmMap & m = *mpm;
    for (Emul::ModuleParmMap::const_iterator it = m.begin(); it != m.end(); ++it) {
        QString pname (it->first.c_str());
        const Emul::ModuleParm & p = it->second;
        if (p.type != Emul::ModuleParm::Int) continue;
        int & i = *const_cast<int *>(reinterpret_cast<const int *>(p.p));
        ParmLineEditMap::const_iterator pit = plem.find(pname);
        if (pit == plem.end()) continue;
        bool ok = false;
        int tmp = pit->second->text().toInt(&ok);
        if (ok) i = tmp;
    }
}

