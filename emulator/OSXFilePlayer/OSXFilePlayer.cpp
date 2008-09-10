/*
  This code takes an audio file and uses the FilePlayer AU to play this to the default output unit
  
  It takes any valid audio file of any format that is understood by the AudioFile API
*/

#include <AudioToolbox/AudioToolbox.h>
#include "CAXException.h"
#include "CAStreamBasicDescription.h"
#include "CAAudioUnit.h"
#include "AUOutputBL.cpp"
#include "CAAudioChannelLayout.cpp"
#include "CAAudioChannelLayoutObject.cpp"
#include "CAAudioUnit.cpp"
#include "CACFDictionary.cpp"
#include "CAComponent.cpp"
#include "CAComponentDescription.cpp"
#include "CAStreamBasicDescription.cpp"
#include "CAXException.cpp"
#include "OSXFilePlayer.h"
#include <string>

// helper functions
static double PrepareFileAU (CAAudioUnit &au, CAStreamBasicDescription &fileFormat, AudioFileID audioFile, unsigned loopct) throw(const CAXException &);
static void MakeSimpleGraph (AUGraph &theGraph, CAAudioUnit &fileAU, CAStreamBasicDescription &fileFormat, AudioFileID audioFile) throw(const CAXException &);

struct OSXFilePlayer::Impl
{
    AudioFileID audioFile;
    FSRef theRef;
    CAStreamBasicDescription fileFormat;
    AUGraph theGraph;
    CAAudioUnit fileAU;
    double fileDuration;
    bool needstop, needuninit, needclose, needgraphclose;
    std::string filename;
    unsigned loopct;

    Impl() : needstop(false), needuninit(false), needclose(false), needgraphclose(false) {}
    
    ~Impl() 
    {
        // lets clean up
//         if (needstop)   XThrowIfError (AUGraphStop (theGraph), "AUGraphStop");
//         XThrowIfError (AUGraphUninitialize (theGraph), "AUGraphUninitialize");
//         XThrowIfError (AudioFileClose (audioFile), "AudioFileClose");
//         XThrowIfError (AUGraphClose (theGraph), "AUGraphClose");
        if (needstop)   AUGraphStop(theGraph);
        if (needuninit) AUGraphUninitialize(theGraph);
        if (needclose)  AudioFileClose(audioFile);
        if (needgraphclose) AUGraphClose(theGraph);
    }
};

OSXFilePlayer::OSXFilePlayer()
   : p(0)
{
}

OSXFilePlayer::~OSXFilePlayer()
{
    if (p) delete p, p = 0;
}

void OSXFilePlayer::play() throw(const CAXException &)
{
    if (!p) return;
    else {
        std::string fname = p->filename;
        unsigned loopct = p->loopct;
        delete p;
        p = new Impl;
        p->filename = fname;
        p->loopct = loopct;
    }

    XThrowIfError (FSPathMakeRef ((const UInt8 *)p->filename.c_str(), &p->theRef, NULL), "FSPathMakeRef");

    XThrowIfError (AudioFileOpen (&p->theRef, fsRdPerm, 0, &p->audioFile), "AudioFileOpen");
    p->needclose = true;

    // get the number of channels of the file
    UInt32 propsize = sizeof(CAStreamBasicDescription);
    XThrowIfError (AudioFileGetProperty(p->audioFile, kAudioFilePropertyDataFormat, &propsize, &p->fileFormat), "AudioFileGetProperty");
	
    //    printf ("playing file: %s\n", inputFile);
    //    printf ("format: "); fileFormat.Print();


    // this makes the graph, the file AU and sets it all up for playing
    MakeSimpleGraph (p->theGraph, p->fileAU, p->fileFormat, p->audioFile);
    p->needuninit = true;
    p->needgraphclose = true;

    // now we load the file contents up for playback before we start playing
    // this has to be done the AU is initialized and anytime it is reset or uninitialized
    p->fileDuration = PrepareFileAU (p->fileAU, p->fileFormat, p->audioFile, p->loopct);
    //printf ("file duration: %f secs\n", fileDuration);
	
    // sleep until the file is finished
    //usleep ((int)fileDuration * 1000 * 1000);

    // start playing
    XThrowIfError (AUGraphStart (p->theGraph), "AUGraphStart");
    p->needstop = true;
}

void OSXFilePlayer::stop() throw (const CAXException &)
{
    if (!p) return;
    if (p->needstop)
        XThrowIfError (AUGraphStop (p->theGraph), "AUGraphStop");
    p->needstop = false;
}

void OSXFilePlayer::setFile(const char *name, unsigned loopct) 
{
    if (p) delete p, p = 0;
    p = new Impl;

    p->filename = name;
    p->loopct = loopct;
}

double OSXFilePlayer::duration() const
{
    if (p) return p->fileDuration;
    return 0.0;
}

unsigned OSXFilePlayer::loopCount() const
{
    if (p) return p->loopct;
    return 0;
}

const char *OSXFilePlayer::fileName() const
{
    if (p) return p->filename.c_str();
    return "";
}

static double PrepareFileAU (CAAudioUnit &au, CAStreamBasicDescription &fileFormat, AudioFileID audioFile, unsigned loopct) throw(const CAXException &)
{	
    // 
    // calculate the duration
    UInt64 nPackets;
    UInt32 propsize = sizeof(nPackets);
    XThrowIfError (AudioFileGetProperty(audioFile, kAudioFilePropertyAudioDataPacketCount, &propsize, &nPackets), "kAudioFilePropertyAudioDataPacketCount");
		
    Float64 fileDuration = (nPackets * fileFormat.mFramesPerPacket) / fileFormat.mSampleRate;

    ScheduledAudioFileRegion rgn;
    memset (&rgn.mTimeStamp, 0, sizeof(rgn.mTimeStamp));
    rgn.mTimeStamp.mFlags = kAudioTimeStampSampleTimeValid;
    rgn.mTimeStamp.mSampleTime = 0;
    rgn.mCompletionProc = NULL;
    rgn.mCompletionProcUserData = NULL;
    rgn.mAudioFile = audioFile;
    rgn.mLoopCount = loopct;
    rgn.mStartFrame = 0;
    rgn.mFramesToPlay = UInt32(nPackets * fileFormat.mFramesPerPacket);
		
    // tell the file player AU to play all of the file
    XThrowIfError (au.SetProperty (kAudioUnitProperty_ScheduledFileRegion, 
                                   kAudioUnitScope_Global, 0,&rgn, sizeof(rgn)), "kAudioUnitProperty_ScheduledFileRegion");
	
    // prime the fp AU with default values
    UInt32 defaultVal = 0;
    XThrowIfError (au.SetProperty (kAudioUnitProperty_ScheduledFilePrime, 
                                   kAudioUnitScope_Global, 0, &defaultVal, sizeof(defaultVal)), "kAudioUnitProperty_ScheduledFilePrime");

    // tell the fp AU when to start playing (this ts is in the AU's render time stamps; -1 means next render cycle)
    AudioTimeStamp startTime;
    memset (&startTime, 0, sizeof(startTime));
    startTime.mFlags = kAudioTimeStampSampleTimeValid;
    startTime.mSampleTime = -1;
    XThrowIfError (au.SetProperty(kAudioUnitProperty_ScheduleStartTimeStamp, 
                                  kAudioUnitScope_Global, 0, &startTime, sizeof(startTime)), "kAudioUnitProperty_ScheduleStartTimeStamp");

    return fileDuration;
}



static void MakeSimpleGraph (AUGraph &theGraph, CAAudioUnit &fileAU, CAStreamBasicDescription &fileFormat, AudioFileID audioFile) throw(const CAXException &)
{
    XThrowIfError (NewAUGraph (&theGraph), "NewAUGraph");
	
    CAComponentDescription cd;

    // output node
    cd.componentType = kAudioUnitType_Output;
    cd.componentSubType = kAudioUnitSubType_DefaultOutput;
    cd.componentManufacturer = kAudioUnitManufacturer_Apple;

    AUNode outputNode;
    XThrowIfError (AUGraphNewNode (theGraph, &cd, 0, NULL, &outputNode), "AUGraphNewNode");
	
    // file AU node
    AUNode fileNode;
    cd.componentType = kAudioUnitType_Generator;
    cd.componentSubType = kAudioUnitSubType_AudioFilePlayer;
	
    XThrowIfError (AUGraphNewNode (theGraph, &cd, 0, NULL, &fileNode), "AUGraphNewNode");
	
    // connect & setup
    XThrowIfError (AUGraphOpen (theGraph), "AUGraphOpen");
	
    // install overload listener to detect when something is wrong
    AudioUnit anAU;
    XThrowIfError (AUGraphGetNodeInfo(theGraph, fileNode, NULL, NULL, NULL, &anAU), "AUGraphGetNodeInfo");
	
    fileAU = CAAudioUnit (fileNode, anAU);

    // prepare the file AU for playback
    // set its output channels
    XThrowIfError (fileAU.SetNumberChannels (kAudioUnitScope_Output, 0, fileFormat.NumberChannels()), "SetNumberChannels");

    // load in the file 
    XThrowIfError (fileAU.SetProperty(kAudioUnitProperty_ScheduledFileIDs, 
                                      kAudioUnitScope_Global, 0, &audioFile, sizeof(audioFile)), "SetScheduleFile");


    XThrowIfError (AUGraphConnectNodeInput (theGraph, fileNode, 0, outputNode, 0), "AUGraphConnectNodeInput");

    // AT this point we make sure we have the file player AU initialized
    // this also propogates the output format of the AU to the output unit
    XThrowIfError (AUGraphInitialize (theGraph), "AUGraphInitialize");
	
    // workaround a race condition in the file player AU
    usleep (10 * 1000);

    // if we have a surround file, then we should try to tell the output AU what the order of the channels will be
    if (fileFormat.NumberChannels() > 2) {
        UInt32 layoutSize = 0;
        OSStatus err;
        XThrowIfError (err = AudioFileGetPropertyInfo (audioFile, kAudioFilePropertyChannelLayout, &layoutSize, NULL),
                       "kAudioFilePropertyChannelLayout");
		
        if (!err && layoutSize) {
            char* layout = new char[layoutSize];
			
            err = AudioFileGetProperty(audioFile, kAudioFilePropertyChannelLayout, &layoutSize, layout);
            XThrowIfError (err, "Get Layout From AudioFile");
			
            // ok, now get the output AU and set its layout
            XThrowIfError (AUGraphGetNodeInfo(theGraph, outputNode, NULL, NULL, NULL, &anAU), "AUGraphGetNodeInfo");
			
            err = AudioUnitSetProperty (anAU, kAudioUnitProperty_AudioChannelLayout, 
                                        kAudioUnitScope_Input, 0, layout, layoutSize);
            XThrowIfError (err, "kAudioUnitProperty_AudioChannelLayout");
			
            delete [] layout;
        }
    }
}
