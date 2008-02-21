#ifndef WavFile_H
#define WavFile_H

class OWavFile
{
public:
  OWavFile();
  ~OWavFile();

  bool create(const char *filename, unsigned n_chans = 2, unsigned bits_per_sample = 16, unsigned srate = 44100);
  void close();
  bool isOk() const;
  bool isOpen() const;

  // some properties passed in at creation time
  unsigned srate() const;
  unsigned nchans() const;
  unsigned bits() const; 

  /// size of the entire data portion of the file thus far in bytes (sampSizeBytes()*sizeSamps())
  unsigned sizeBytes() const;
  /// size of the entire data portion of the file thus far in sample frames
  unsigned sizeSamps() const;
  /// size of each sample frame in bytes (nchans * sample_bits/8)
  unsigned sampSizeBytes() const;

  // NB: when writing data, channels should be interleaved as in sample0{chan0,chan1},sample1{chan0,chan1} etc..
  bool write(const double *data,  /**< sample data.. channels are interleaved*/
             unsigned n_datas /**< size in terms of number of individual samples, not number of sample frames */, 
             unsigned this_data_srate, /**< we resample if this srate is different from file srate */
             double scale_min = -1., double scale_max = 1.);

private:
  struct Impl;
  Impl *p;
};

#endif
