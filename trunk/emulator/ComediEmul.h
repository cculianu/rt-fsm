#ifndef ComediEmul_H
#define ComediEmul_H
#include "kernel_emul.h"

class ComediEmul
{
public:
    static ComediEmul *getInstance(const char *devfile);
    static void putInstance(ComediEmul *);

    void putInstance() { putInstance(this); }

    int findByType(int type, unsigned first=0) const;
    int getSubdevType(unsigned sd) const; ///< COMEDI_SUBD_AI, DIO, etc
    int getNSubdevs() const { return dev.nsubdevs; }
    int getNChans(unsigned sd) const;
    int getNRanges(unsigned sd, unsigned ch) const;
    int getKRange(unsigned sd, unsigned ch, unsigned r, comedi_krange *out) const;
    lsampl_t getMaxData(unsigned sd, unsigned ch) const;
    int dioConfig(unsigned sd, unsigned ch, int io);
    int dioGetConfig(unsigned sd, unsigned ch) const;
    int dioBitField(unsigned sd, unsigned mask, unsigned *bits);
    int dataWrite(unsigned sd, unsigned ch, unsigned range, lsampl_t data);
    int dataRead(unsigned sd, unsigned ch, unsigned range, lsampl_t * data) const;
    double read(unsigned sd, unsigned ch) const;
    void   write(unsigned sd, unsigned ch, double v);

    enum Profile {
        NI_6025E,
        NI_6071E,
        NI_6229M,
        N_PROFILES
    };

    // default profiles are /dev/comedi0 through /dev/comedi3 are NI_6025E
    static void setProfile(const char *devfile, Profile profile);
    static Profile getProfile(const char *devfile);

private:

    static const unsigned MAX_RANGES = 16, MAX_CHANS = 256, MAX_SUBDEVS = 16;
    struct Subdev 
    {
        int type;
        unsigned nchans;
        double chanData[MAX_CHANS]; /* voltages for AI/AO chans.. for DIO is usually 0 or nonzero */
        unsigned nranges;
        comedi_krange ranges[MAX_RANGES];
        unsigned maxdata;
        int configs[MAX_CHANS]; /* dio config per chan: one of COMEDI_INPUT or OCOMEDI_OUTPUT */
    };

    struct Dev {
        unsigned nsubdevs;
        Subdev subdevs[MAX_SUBDEVS];
    };

    Dev dev;
    int refCount;

    static Dev profiles[N_PROFILES];
            
    ComediEmul(Profile p);
    /// noop.. just made private to prevent unauthorized delete since 
    /// you should use getInstance() to create and putInstance() to delete 
    ~ComediEmul();
};



#endif
