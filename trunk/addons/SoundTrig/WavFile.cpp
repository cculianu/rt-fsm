
#include "WavFile.h"
#include <fstream>
#include "IntTypes.h"
#include <string.h>

namespace 
{
  bool isBigEndian();
  int16 LE(int16);
  int32 LE(int32);
  uint32 LE(uint32 x) { return LE(*reinterpret_cast<int32 *>(&x)); }
  uint16 LE(uint16 x) { return LE(*reinterpret_cast<int16 *>(&x)); }
  int16 fromLE(int16);
  int32 fromLE(int32);
  uint32 fromLE(uint32 x) { return fromLE(*reinterpret_cast<int32 *>(&x)); }
  uint16 fromLE(uint16 x) { return fromLE(*reinterpret_cast<int16 *>(&x)); }
}

struct OWavFile::Impl
{
  Impl() : nframes(0), bitspersample(0), srate(0), nchans(0) {}
  unsigned nframes, bitspersample, srate, nchans;
  static const unsigned dataSizeOffset = 40, fileSizeOffset = 4;
  std::ofstream f;
};

// some properties passed in at creation time
unsigned OWavFile::srate() const { return p->srate; }
unsigned OWavFile::nchans() const { return p->nchans; }
unsigned OWavFile::bits() const { return p->bitspersample; }
unsigned OWavFile::sizeBytes() const { return sampSizeBytes()*sizeSamps(); }
unsigned OWavFile::sizeSamps() const { return p->nframes; }
unsigned OWavFile::sampSizeBytes() const { return nchans()*(bits()/8); }


struct Header {
  char riff[4]; // 'RIFF'
  uint32 fileLength; // size of rest of file not including first 8 bytes
  char wave[4]; // 'WAVE'

  Header() { memcpy(riff, "RIFF", 4); memcpy(wave, "WAVE", 4); fileLength = LE(0); }
};

struct FormatChunk {
  char           chunkID[4]; // not zero terminated!, should be "fmt "
  int32          chunkSize; // does not include first 8 bytes used by first 2 fields.. refers to size of remaining fields and should be set to 16 for wFormatTag = 1

  int16          wFormatTag; // set it to 1
  uint16         wChannels; 
  uint32         dwSamplesPerSec; /* samplerate in Hz */
  uint32         dwAvgBytesPerSec; /* = wBlockAlign * dwSamplesPerSec */
  uint16         wBlockAlign; // = wChannels * (wBitsPerSample / 8)
  uint16         wBitsPerSample; // standard rates are 11025 22050 and 44100
/* Note: there may be additional fields here, depending upon wFormatTag. */

  FormatChunk() 
  { 
    memcpy(chunkID, "fmt ", 4); 
    chunkSize = LE(16); 
    wFormatTag = LE(1);   
    dwSamplesPerSec = dwAvgBytesPerSec = wBlockAlign = wBitsPerSample = wChannels = 0;
  }
};

struct DataChunk
{
  char          chunkID[4];
  int32         chunkSize; // does not include the first 8 bytes
  // data goes here...
  DataChunk() { memcpy(chunkID, "data", 4); chunkSize = LE(0); }
};

OWavFile::OWavFile()
{
  p = new Impl;
}

OWavFile::~OWavFile()
{
  close();
  delete p;
  p = 0;
}

bool OWavFile::isOpen() const
{
  return p->f.is_open();
}

bool OWavFile::isOk() const
{
  return isOpen() && p->f.good();
}

void OWavFile::close()
{
  if (isOpen()) {
    // finalize -- commit the file size and data size to the proper offsets
    uint32 dataBytes = sizeBytes(), tmp;
    p->f.seekp(Impl::dataSizeOffset);
    tmp = LE(dataBytes);
    p->f.write((const char *)&tmp, sizeof(tmp));
    p->f.seekp(Impl::fileSizeOffset);
    tmp = LE(uint32(dataBytes + sizeof(DataChunk) + sizeof(FormatChunk) + sizeof(Header) - sizeof(int32)*2));
    p->f.write((const char *)&tmp, sizeof(tmp));    
    p->f.close();
  }
}

bool OWavFile::create(const char *filename, unsigned n_chans, unsigned bits_per_sample, unsigned srate)
{
  if (srate != 44100 && srate != 22050 && srate != 11025) return false; // invalid sample rate
  if (bits_per_sample != 8 && bits_per_sample != 16 && bits_per_sample != 24 && bits_per_sample != 32) return false;
  if (n_chans < 1 || n_chans > 2) return false;
  p->f.open(filename, std::ios_base::out|std::ios_base::trunc|std::ios_base::binary);
  if (!p->f.is_open()) return false;
  p->srate = srate;
  p->nchans = n_chans;
  p->bitspersample = bits_per_sample;
  Header h;
  FormatChunk f;
  DataChunk d;
  f.wChannels = LE(n_chans);
  f.dwSamplesPerSec = LE(srate);
  f.wBlockAlign = LE(n_chans * bits_per_sample/8);
  f.dwAvgBytesPerSec = LE(srate * n_chans * bits_per_sample/8);
  f.wBitsPerSample = LE(bits_per_sample);
  p->f.write((const char *)&h, sizeof(Header));
  p->f.write((const char *)&f, sizeof(FormatChunk));
  p->f.write((const char *)&d, sizeof(DataChunk));
  return isOk();
}

namespace 
{
  inline double rnd(double d) 
  { 
      if (d < 0.) 
          return double(unsigned(d-0.5)); // round away from 0 for negative
      return double(unsigned(d+0.5)); // round away from 0 for positive
  }
}

// NB: when writing data, channels should be interleaved as in sample0{chan0,chan1},sample1{chan0,chan1} etc..
bool OWavFile::write(const double *data, unsigned size, unsigned srate, double scale_min, double scale_max)
{
  double factor = 1.0;

  // NB: need to upsample/downsample here.. ergh we use a scale factor that turns out to be how much we increment our frame loop index by below..
  if (srate != p->srate) {
    factor = double(srate)/double(p->srate);    
  }

  unsigned nwrit = 0, nframes = size/p->nchans;
  for (double i = 0; i < nframes; i += factor, ++nwrit) {
    for (unsigned chan = 0; chan < p->nchans; ++chan) {
      unsigned idx = unsigned(rnd(i))*p->nchans+chan;
      uint32 samp = uint32((data[idx]-scale_min)/(scale_max-scale_min) * double(~0U)); // 32-bit unsigned sample
      if (p->bitspersample != 8) {
        // argh, deal with signed PCM
        samp -= 0x7fffffff;
      }
      samp = samp >> (32-p->bitspersample); // take the upper BitsPerSample bits
      samp = LE(samp); // make sure it's little endian so that the write command below 1. succeeds on all platforms (we write the first bitspersaple bits) and 2. wave files are anyway little endian
      p->f.write((const char *)&samp, p->bitspersample/8);
    }
    if (!p->f.good()) break;
  }
  p->nframes += nwrit;
  return isOk();
}

bool OWavFile::write(const void *v,
                     unsigned size, 
                     unsigned bits,
                     unsigned srate)
{
  const uint8 *d = static_cast<const uint8 *>(v);  
  double factor = 1.0, scale_min, scale_range;

  switch (bits) {
    case 8: scale_min = 0., scale_range = 256.; break;
    case 16: scale_min = -32768., scale_range = 65536.; break;
    case 24: scale_min = -8388608., scale_range = 16777216.; break;
    case 32: scale_min = -2147483648., scale_range = 4294967296.; break;
    default: return false;
  }
  
  // NB: need to upsample/downsample here.. ergh we use a scale factor that turns out to be how much we increment our frame loop index by below..
  if (srate != p->srate) {
    factor = double(srate)/double(p->srate);    
  }
  
  unsigned nwrit = 0, nframes = size/p->nchans;
  for (double i = 0; i < nframes; i += factor, ++nwrit) {
    for (unsigned chan = 0; chan < p->nchans; ++chan) {
      unsigned idx = unsigned(rnd(i))*p->nchans+chan;
      uint32 samp;
      double datum;
      switch (bits) {
        case 8:  datum = d[idx]; break;
        case 16: datum = ((int16 *)d)[idx]; break;
        case 24: 
          samp = 0;
          memcpy(&samp, d+idx*3, 3);
          if (isBigEndian()) samp >>= 8;
          datum = samp;
          break;
        case 32: datum = ((int32 *)d)[idx]; break;
      }
      samp = uint32((datum-scale_min)/(scale_range) * double(~0U));
      if (p->bitspersample != 8) {
        // argh, deal with signed PCM
        samp -= 0x7fffffff;
      }
      samp = samp >> (32-p->bitspersample); // take the upper BitsPerSample bits
      samp = LE(samp); // make sure it's little endian so that the write command below 1. succeeds on all platforms (we write the first bitspersaple bits) and 2. wave files are anyway little endian
      p->f.write((const char *)&samp, p->bitspersample/8);
    }
    if (!p->f.good()) break;
  }
  p->nframes += nwrit;
  return isOk();
}

namespace 
{
  bool isBigEndian()
  {
    static const uint32 dword = 0x12345678;
    static const uint8 * dp = reinterpret_cast<const uint8 *>(&dword);
    // little endian machines store the dword as 0x78563412
    // big endian store it as written by humans MSB first
    return dp[0] == 0x12;
  }
  int16 LE(int16 x) 
  {   
    if (isBigEndian()) {
      int8 * p = (int8 *)&x;
      p[0] ^= p[1]; // swap the two bytes
      p[1] ^= p[0];
      p[0] ^= p[1];      
    }
    return x;
  }
  int32 LE(int32 x)
  {
    if (isBigEndian()) {
      int8 * p = (int8 *)&x;
      p[0] ^= p[3]; // swap the two end bytes
      p[3] ^= p[0];
      p[0] ^= p[3];      

      p[1] ^= p[2]; // swap the two middle bytes
      p[2] ^= p[1];
      p[1] ^= p[2];      
    }
    return x;    
  }

  int16 fromLE(int16 x) { return LE(x); }
  int32 fromLE(int32 x) { return LE(x); }
}
