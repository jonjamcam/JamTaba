#include "NinjamTrackNode.h"
#include "audio/core/AudioDriver.h"
#include <QDataStream>
#include <QDebug>
#include <QList>
#include <QByteArray>
#include <QMutexLocker>
#include <QDateTime>
#include <QtConcurrent/QtConcurrent>
#include "audio/core/Filters.h"

const double NinjamTrackNode::LOW_CUT_FREQUENCY = 220.0; // in Hertz

class NinjamTrackNode::LowCutFilter
{
public:
    LowCutFilter(double sampleRate);
    void process(Audio::SamplesBuffer &buffer);
    inline bool isActivated() const { return activated; }
    inline void setActivated(bool activated){ this->activated = activated; }
private:
    bool activated;
    Audio::Filter leftFilter;
    Audio::Filter rightFilter;
};

NinjamTrackNode::LowCutFilter::LowCutFilter(double sampleRate) :
    activated(false),
    leftFilter(Audio::Filter::FilterType::HighPass, sampleRate, LOW_CUT_FREQUENCY, 1.0, 1.0),
    rightFilter(Audio::Filter::FilterType::HighPass, sampleRate, LOW_CUT_FREQUENCY, 1.0, 1.0)
{

}

void NinjamTrackNode::LowCutFilter::process(Audio::SamplesBuffer &buffer)
{
    quint32 samples = buffer.getFrameLenght();
    leftFilter.process(buffer.getSamplesArray(0), samples);
    if (!buffer.isMono())
        rightFilter.process(buffer.getSamplesArray(1), samples);
}

//--------------------------------------------------------------------------

class NinjamTrackNode::IntervalDecoder
{
public:
    IntervalDecoder(const QByteArray &vorbisData);
    void decode(quint32 maxSamplesToDecode);
    quint32 getDecodedSamples(Audio::SamplesBuffer &outBuffer, int samplesToDecode);
    inline int getSampleRate() { return vorbisDecoder.getSampleRate(); }
private:
    VorbisDecoder vorbisDecoder;
    Audio::SamplesBuffer decodedBuffer;
    QMutex mutex;
};

NinjamTrackNode::IntervalDecoder::IntervalDecoder(const QByteArray &vorbisData)
    :decodedBuffer(2)
{
    vorbisDecoder.setInputData(vorbisData);
}

void NinjamTrackNode::IntervalDecoder::decode(quint32 maxSamplesToDecode)
{
    mutex.lock();

    decodedBuffer.append(vorbisDecoder.decode(maxSamplesToDecode));

    mutex.unlock();
}

quint32 NinjamTrackNode::IntervalDecoder::getDecodedSamples(Audio::SamplesBuffer &outBuffer, int samplesToDecode)
{
    mutex.lock();
    while (decodedBuffer.getFrameLenght() < samplesToDecode) { //need decode more samples to fill outBuffer?
        quint32 toDecode = samplesToDecode - decodedBuffer.getFrameLenght();
        const Audio::SamplesBuffer &decodedSamples = vorbisDecoder.decode(toDecode);
        decodedBuffer.append(decodedSamples);
        if (decodedSamples.isEmpty())
            break; //no more samples to decode
    }

    quint32 totalSamples = qMin(samplesToDecode, decodedBuffer.getFrameLenght());
    outBuffer.setFrameLenght(totalSamples);
    outBuffer.set(decodedBuffer);
    decodedBuffer.discardFirstSamples(totalSamples);
    mutex.unlock();
    return totalSamples;
}

//-------------------------------------------------------------

NinjamTrackNode::NinjamTrackNode(int ID) :
    ID(ID),
    processingLastPartOfInterval(false),
    currentDecoder(nullptr),
    decodersMutex(QMutex::NonRecursive),
    lowCut(new NinjamTrackNode::LowCutFilter(44100))
{

}

void NinjamTrackNode::setLowCutStatus(bool activated)
{
    lowCut->setActivated(activated);
}

int NinjamTrackNode::getSampleRate() const
{
    if (currentDecoder)
        return currentDecoder->getSampleRate();
    return 44100;
}

NinjamTrackNode::~NinjamTrackNode()
{
    decodersMutex.lock();
    foreach (IntervalDecoder *decoder, decoders)
        delete decoder;

    decoders.clear();
    if (currentDecoder) {
        delete currentDecoder;
        currentDecoder = nullptr;
    }
    decodersMutex.unlock();
}

void NinjamTrackNode::discardIntervals(bool keepMostRecentInterval)
{
    decodersMutex.lock();
    if (!keepMostRecentInterval) {
        while (!decoders.isEmpty())
            delete decoders.takeFirst();
    } else {
        while(decoders.size() > 1)//keep the last downloaded interval
            delete decoders.takeFirst();
    }
    qDebug() << "intervals discarded";
    decodersMutex.unlock();
}

bool NinjamTrackNode::isPlaying()
{
    QMutexLocker locker(&decodersMutex);
    return currentDecoder != nullptr;
}

bool NinjamTrackNode::startNewInterval()
{
    decodersMutex.lock();
    if (currentDecoder) {
        delete currentDecoder; //discard the precious interval decoder
        currentDecoder = nullptr;
    }
    if (!decoders.isEmpty())
        currentDecoder = decoders.takeFirst(); //using the next buffered decoder (next interval)

    decodersMutex.unlock();
    return isPlaying();
}

void NinjamTrackNode::addVorbisEncodedInterval(const QByteArray &vorbisData)
{
    decodersMutex.lock();
    IntervalDecoder *newIntervalDecoder = new IntervalDecoder(vorbisData);
    decoders.append(newIntervalDecoder);
    decodersMutex.unlock();

    //decoding the first samples in a separated thread to avoid slow down the audio thread in interval start (first beat)
    QtConcurrent::run(newIntervalDecoder, &NinjamTrackNode::IntervalDecoder::decode, 256);
}

// ++++++++++++++++++++++++++++++++++++++

int NinjamTrackNode::getFramesToProcess(int targetSampleRate, int outFrameLenght)
{
    return needResamplingFor(targetSampleRate) ? getInputResamplingLength(
        getSampleRate(), targetSampleRate, outFrameLenght) : outFrameLenght;
}

void NinjamTrackNode::processReplacing(const Audio::SamplesBuffer &in, Audio::SamplesBuffer &out,
                                       int sampleRate, const Midi::MidiMessageBuffer &midiBuffer)
{
    if (!isPlaying())
        return;

    Q_ASSERT(currentDecoder);
    int framesToProcess = getFramesToProcess(sampleRate, out.getFrameLenght());
    internalInputBuffer.setFrameLenght(framesToProcess);
    currentDecoder->getDecodedSamples(internalInputBuffer, framesToProcess);

    if (!internalInputBuffer.isEmpty()) {
        if (needResamplingFor(sampleRate)) {
            const Audio::SamplesBuffer &resampledBuffer = resampler.resample(internalInputBuffer,
                                                                             out.getFrameLenght());
            internalInputBuffer.setFrameLenght(resampledBuffer.getFrameLenght());
            internalInputBuffer.set(resampledBuffer);
            if (internalInputBuffer.getFrameLenght() != out.getFrameLenght())
                qWarning() << internalInputBuffer.getFrameLenght() << " != "
                           << out.getFrameLenght();
        }

        if (lowCut->isActivated())
            lowCut->process(internalInputBuffer);

        Audio::AudioNode::processReplacing(in, out, sampleRate, midiBuffer);// process internal buffer pan, gain, etc
    }
}

bool NinjamTrackNode::needResamplingFor(int targetSampleRate) const
{
    if (currentDecoder)
        return currentDecoder->getSampleRate() != targetSampleRate;
    return false;
}
