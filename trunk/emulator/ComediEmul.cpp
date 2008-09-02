#include "ComediEmul.h"
#include <map>
#include <string>
#include <string.h>

#define RANGE(a,b)              {static_cast<int>((a)*1e6),static_cast<int>((b)*1e6),0}
// profiles
ComediEmul::Dev ComediEmul::profiles[ComediEmul::N_PROFILES] = 
{ /* array of ComediEmul::Dev */
        { /* NI_6025E */
            4, /* nsubdevs */
            { /* array of struct ComediEmul::Subdev */
                { /* struct ComediEmul::Subdev */
                    COMEDI_SUBD_AI,
                    16,
                    { 0. },
                    4,
                    {
                        RANGE(-10, 10),
                        RANGE(-5, 5),
                        RANGE(-0.5, -0.5),
                        RANGE(-0.05, 0.05)
                    },
                    (1<<12)-1,
                    {0}
                },
                { /* struct ComediEmul::Subdev */
                    COMEDI_SUBD_AO,
                    2,
                    { 0. },
                    1,
                    {
                        RANGE(-10, 10),
                    },
                    (1<<12)-1,
                    {0}
                },
                {
                    COMEDI_SUBD_DIO,
                    8,
                    { 0. },
                    1,
                    {
                        RANGE(0, 1)
                    },
                    1,
                    {0}
           
                },
                {
                    COMEDI_SUBD_DIO,
                    24,
                    { 0. },
                    1,
                    {
                        RANGE(0, 1)
                    },
                    1,
                    {0}
                }
            }
        },
        { /* NI_6071E */
            4, /* nsubdevs */
            { /* array of struct ComediEmul::Subdev */
                { /* struct ComediEmul::Subdev */
                    COMEDI_SUBD_AI,
                    64,
                    { 0. },
                    16,
                    {
                        RANGE(-10, 10),
                        RANGE(-5, 5),
                        RANGE(-2.5, 2.5),
                        RANGE(-1, 1),
                        RANGE(-0.5, 0.5),
                        RANGE(-0.25, 0.25),
                        RANGE(-0.1, 0.1),
                        RANGE(-0.05, 0.05),
                        RANGE(0, 20),
                        RANGE(0, 10),
                        RANGE(0, 5),
                        RANGE(0, 2),
                        RANGE(0, 1),
                        RANGE(0, 0.5),
                        RANGE(0, 0.2),
                        RANGE(0, 0.1)
                    },
                    (1<<12)-1,
                    {0}
                },
                { /* struct ComediEmul::Subdev */
                    COMEDI_SUBD_AO,
                    2,
                    { 0. },
                    4,
                    {
                        RANGE(-10, 10),
                        RANGE(0, 10),
                        RANGE(-1, 1),
                        RANGE(0, 1)
                    },
                    (1<<12)-1,
                    {0}
                },
                {
                    COMEDI_SUBD_DIO,
                    8,
                    { 0. },
                    1,
                    {
                        RANGE(0, 1)
                    },
                    1,
                    {0}                  
                },
                {
                    COMEDI_SUBD_DIO,
                    24,
                    { 0. },
                    1,
                    {
                        RANGE(0, 1)
                    },
                    1,
                    {0}                  
                }
            }
        },
        { /* NI_6229E */
            3, /* nsubdevs */
            { /* array of struct ComediEmul::Subdev */
                { /* struct ComediEmul::Subdev */
                    COMEDI_SUBD_AI,
                    32,
                    { 0. },
                    4,
                    {
                        RANGE(-10, 10),
                        RANGE(-5, 5),
                        RANGE(-0.5, 0.5),
                        RANGE(-0.25, 0.25)
                    },
                    (1<<16)-1,
                    {0}
                },
                { /* struct ComediEmul::Subdev */
                    COMEDI_SUBD_AO,
                    2,
                    { 0. },
                    4,
                    {
                        RANGE(-10, 10),
                        RANGE(-5, 5),
                        RANGE(-1, 1),
                        RANGE(-0.2, 0.2)
                    },
                    (1<<16)-1,
                    {0}
                },
                {
                    COMEDI_SUBD_DIO,
                    32,
                    { 0. },
                    1,
                    {
                        RANGE(0, 1)
                    },
                    1,
                    {0}                  
                }
            }
        }
};

typedef std::map<std::string, ComediEmul::Profile> ProfileMap;
static ProfileMap profileMap;

void ComediEmul::setProfile(const char *devfile, Profile p)
{
    if (p < N_PROFILES) 
        profileMap[devfile] = p;
}

ComediEmul::Profile ComediEmul::getProfile(const char *devfile)
{
    return profileMap[devfile];
}

typedef std::map<std::string, ComediEmul *> InstanceMap;
static InstanceMap instances;

ComediEmul::ComediEmul(Profile p)
    : refCount(0)
{
    if (p < N_PROFILES) memcpy(&dev, &profiles[p], sizeof(Dev));    
}

ComediEmul::~ComediEmul() { /* noop, but private */ }

ComediEmul *ComediEmul::getInstance(const char *devfile)
{
    InstanceMap::iterator it = instances.find(devfile);
    if (it == instances.end()) {
        instances.insert(InstanceMap::value_type(devfile, new ComediEmul(profileMap[devfile])));
        it = instances.find(devfile);
    }
    ++it->second->refCount;
    return it->second;
}

void ComediEmul::putInstance(ComediEmul *c) 
{
    if (--c->refCount <= 0) {
        InstanceMap::iterator it;
        for (it = instances.begin(); it != instances.end(); ++it) {
            if (it->second == c) {
                delete c;
                instances.erase(it);
                return;
            }
        }
    }
}

int ComediEmul::findByType(int type, unsigned first) const
{
    for (unsigned i = first; i < dev.nsubdevs; ++i)
        if (dev.subdevs[i].type == type) return i;
    return -1;
}

int ComediEmul::getNChans(unsigned sd) const
{
    if (sd < dev.nsubdevs) {
        return dev.subdevs[sd].nchans;
    }
    return -1;
}

int ComediEmul::getNRanges(unsigned sd, unsigned ch) const
{
    (void)ch;
    if (sd < dev.nsubdevs) {
        return dev.subdevs[sd].nranges;
    }
    return -1;
}

int ComediEmul::getKRange(unsigned sd, unsigned ch, unsigned r, comedi_krange *out) const
{
    (void)ch;
    if (sd < dev.nsubdevs && r < dev.subdevs[sd].nranges) {
        memcpy(out, &dev.subdevs[sd].ranges[r], sizeof(*out));
        return 0;
    }
    return -1;    
}
lsampl_t ComediEmul::getMaxData(unsigned sd, unsigned ch) const
{
    (void)ch;
    if (sd < dev.nsubdevs) {
        return dev.subdevs[sd].maxdata;
    }
    return 0;
}
int ComediEmul::dioConfig(unsigned sd, unsigned ch, int io)
{
    if (sd < dev.nsubdevs && dev.subdevs[sd].type == COMEDI_SUBD_DIO
        && ch < dev.subdevs[sd].nchans
        && (io == COMEDI_INPUT || io == COMEDI_OUTPUT)) {
        dev.subdevs[sd].configs[ch] = io;
        return 0;
    }
    return -1;
}

int ComediEmul::dioGetConfig(unsigned sd, unsigned ch) const
{
    if (sd < dev.nsubdevs && dev.subdevs[sd].type == COMEDI_SUBD_DIO
        && ch < dev.subdevs[sd].nchans) {
        return dev.subdevs[sd].configs[ch];
    }
    return -1;
}

int ComediEmul::dioBitField(unsigned sd, unsigned mask, unsigned *bits)
{
    unsigned & b = *bits;
    int ret = 0;
    if (sd < dev.nsubdevs && dev.subdevs[sd].type == COMEDI_SUBD_DIO) {
        for (unsigned i = 0; i < dev.subdevs[sd].nchans; ++i) {
            if ( ((1<<i) & mask) && dev.subdevs[sd].configs[i] == COMEDI_OUTPUT ) {
                dataWrite(sd, i, 0,  (b>>i) & 0x1 );
                ++ret;
            }
            if (dev.subdevs[sd].configs[i] == COMEDI_INPUT) {
                lsampl_t samp = 0;
                dataRead(sd, i, 0, &samp);
                b &= ~(1<<i); // now, clear bit
                b |= (samp?1:0)<<i; // and set it if it came in as set
                ++ret;
            }
        }
        return ret;
    }
    return -1;
}

int ComediEmul::dataWrite(unsigned sd, unsigned ch, unsigned range, lsampl_t data)
{
    if (sd < dev.nsubdevs && ch < dev.subdevs[sd].nchans && range < dev.subdevs[sd].nranges) {
        double min = dev.subdevs[sd].ranges[range].min * 1e-6, max = dev.subdevs[sd].ranges[range].max * 1e-6;
        double maxdata = dev.subdevs[sd].maxdata;
        dev.subdevs[sd].chanData[ch] = (data/maxdata) * (max-min) + min;
        return 1;
    }
    return -1;
}
int ComediEmul::dataRead(unsigned sd, unsigned ch, unsigned range, lsampl_t * data) const
{
    if (sd < dev.nsubdevs && ch < dev.subdevs[sd].nchans && range < dev.subdevs[sd].nranges) {
        double min = dev.subdevs[sd].ranges[range].min * 1e-6, max = dev.subdevs[sd].ranges[range].max * 1e-6;
        double maxdata = dev.subdevs[sd].maxdata;
        *data = static_cast<int>(maxdata * ((dev.subdevs[sd].chanData[ch] - min)/(max-min)));
        return 1;
    }
    return -1;    
}

double ComediEmul::read(unsigned sd, unsigned ch) const
{
    if (sd < dev.nsubdevs && ch < dev.subdevs[sd].nchans)
        return dev.subdevs[sd].chanData[ch];
    return 0.;
}
void   ComediEmul::write(unsigned sd, unsigned ch, double v)
{
    if (sd < dev.nsubdevs && ch < dev.subdevs[sd].nchans)
        dev.subdevs[sd].chanData[ch] = v;
}

int ComediEmul::getSubdevType(unsigned sd) const
{
    if (sd < dev.nsubdevs) return dev.subdevs[sd].type;
    return -1;
}
