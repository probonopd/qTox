/*
    Copyright © 2014-2015 by The qTox Project

    This file is part of qTox, a Qt-based graphical interface for Tox.

    qTox is libre software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    qTox is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with qTox.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "audio.h"
#include "src/core/core.h"
#include "src/core/coreav.h"
#include "src/persistence/settings.h"

#include <QDebug>
#include <QFile>
#include <QMutexLocker>
#include <QPointer>
#include <QThread>
#include <QtMath>
#include <QWaitCondition>

#include <cassert>

#ifdef QTOX_FILTER_AUDIO
#include "audiofilterer.h"
#endif

/**
@internal

@class Audio::Private

@brief Encapsulates private audio framework from public qTox Audio API.
*/
class Audio::Private
{
public:
    Private()
        : minInputGain{-30.0}
        , maxInputGain{30.0}
        , gain{0.0}
        , gainFactor{0.0}
    {
    }

    qreal inputGain() const
    {
        return gain;
    }

    qreal inputGainFactor() const
    {
        return gainFactor;
    }

    void setInputGain(qreal dB)
    {
        gain = qBound(minInputGain, dB, maxInputGain);
        gainFactor = qPow(10.0, (gain / 20.0));
    }

public:
    qreal   minInputGain;
    qreal   maxInputGain;

private:
    qreal   gain;
    qreal   gainFactor;
};

/**
Returns the singleton instance.
*/
Audio& Audio::getInstance()
{
    static Audio instance;
    return instance;
}

Audio::Audio()
    : d{new Private}
    , audioThread{new QThread}
    , alInDev{nullptr}
    , inSubscriptions{0}
    , alOutDev{nullptr}
    , alOutContext{nullptr}
    , alMainSource{0}
    , alMainBuffer{0}
    , outputInitialized{false}
{
    // initialize OpenAL error stack
    alGetError();
    alcGetError(nullptr);

    audioThread->setObjectName("qTox Audio");
    QObject::connect(audioThread, &QThread::finished, audioThread, &QThread::deleteLater);

    moveToThread(audioThread);

#ifdef QTOX_FILTER_AUDIO
    filterer.startFilter(AUDIO_SAMPLE_RATE);
#endif

    connect(&captureTimer, &QTimer::timeout, this, &Audio::doCapture);
    captureTimer.setInterval(AUDIO_FRAME_DURATION/2);
    captureTimer.setSingleShot(false);
    captureTimer.start();
    connect(&playMono16Timer, &QTimer::timeout, this, &Audio::playMono16SoundCleanup);
    playMono16Timer.setSingleShot(true);

    audioThread->start();
}

Audio::~Audio()
{
    audioThread->exit();
    audioThread->wait();
    cleanupInput();
    cleanupOutput();
#ifdef QTOX_FILTER_AUDIO
    filterer.closeFilter();
#endif
    delete d;
}

void Audio::checkAlError() noexcept
{
    const ALenum al_err = alGetError();
    if (al_err != AL_NO_ERROR)
        qWarning("OpenAL error: %d", al_err);
}

void Audio::checkAlcError(ALCdevice *device) noexcept
{
    const ALCenum alc_err = alcGetError(device);
    if (alc_err)
        qWarning("OpenAL error: %d", alc_err);
}

/**
Returns the current output volume (between 0 and 1)
*/
qreal Audio::outputVolume() const
{
    QMutexLocker locker(&audioLock);

    ALfloat volume = 0.0;

    if (alOutDev)
    {
        alGetListenerf(AL_GAIN, &volume);
        checkAlError();
    }

    return volume;
}

/**
Set the master output volume.

@param[in] volume   the master volume (between 0 and 1)
*/
void Audio::setOutputVolume(qreal volume)
{
    QMutexLocker locker(&audioLock);

    volume = std::max(0.0, std::min(volume, 1.0));

    alListenerf(AL_GAIN, static_cast<ALfloat>(volume));
    checkAlError();
}

/**
@brief The minimum gain value for an input device.

@return minimum gain value in dB
*/
qreal Audio::minInputGain() const
{
    QMutexLocker locker(&audioLock);
    return d->minInputGain;
}

/**
@brief Set the minimum allowed gain value in dB.

@note Default is -30dB; usually you don't need to alter this value;
*/
void Audio::setMinInputGain(qreal dB)
{
    QMutexLocker locker(&audioLock);
    d->minInputGain = dB;
}

/**
@brief The maximum gain value for an input device.

@return maximum gain value in dB
*/
qreal Audio::maxInputGain() const
{
    QMutexLocker locker(&audioLock);
    return d->maxInputGain;
}

/**
@brief Set the maximum allowed gain value in dB.

@note Default is 30dB; usually you don't need to alter this value.
*/
void Audio::setMaxInputGain(qreal dB)
{
    QMutexLocker locker(&audioLock);
    d->maxInputGain = dB;
}

/**
@brief The dB gain value.

@return the gain value in dB
*/
qreal Audio::inputGain() const
{
    QMutexLocker locker(&audioLock);
    return d->inputGain();
}

/**
Set the input gain dB level.
*/
void Audio::setInputGain(qreal dB)
{
    QMutexLocker locker(&audioLock);
    d->setInputGain(dB);
}

void Audio::reinitInput(const QString& inDevDesc)
{
    QMutexLocker locker(&audioLock);
    cleanupInput();
    initInput(inDevDesc);
}

bool Audio::reinitOutput(const QString& outDevDesc)
{
    QMutexLocker locker(&audioLock);
    cleanupOutput();
    return initOutput(outDevDesc);
}

/**
@brief Subscribe to capture sound from the opened input device.

If the input device is not open, it will be opened before capturing.
*/
void Audio::subscribeInput()
{
    QMutexLocker locker(&audioLock);

    if (!autoInitInput())
    {
        qWarning("Failed to subscribe to audio input device.");
        return;
    }

    inSubscriptions++;
    qDebug() << "Subscribed to audio input device [" << inSubscriptions << "subscriptions ]";
}

/**
@brief Unsubscribe from capturing from an opened input device.

If the input device has no more subscriptions, it will be closed.
*/
void Audio::unsubscribeInput()
{
    QMutexLocker locker(&audioLock);

    if (!inSubscriptions)
        return;

    inSubscriptions--;
    qDebug() << "Unsubscribed from audio input device [" << inSubscriptions << "subscriptions left ]";

    if (!inSubscriptions)
        cleanupInput();
}

/**
Initialize audio input device, if not initialized.

@return true, if device was initialized; false otherwise
*/
bool Audio::autoInitInput()
{
    return alInDev ? true : initInput(Settings::getInstance().getInDev());
}

/**
Initialize audio output device, if not initialized.

@return true, if device was initialized; false otherwise
*/
bool Audio::autoInitOutput()
{
    return alOutDev ? true : initOutput(Settings::getInstance().getOutDev());
}

bool Audio::initInput(QString inDevDescr)
{
    qDebug() << "Opening audio input" << inDevDescr;

    if (inDevDescr == "none")
        return true;

    assert(!alInDev);

    /// TODO: Try to actually detect if our audio source is stereo
    int stereoFlag = AUDIO_CHANNELS == 1 ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16;
    const uint32_t sampleRate = AUDIO_SAMPLE_RATE;
    const uint16_t frameDuration = AUDIO_FRAME_DURATION;
    const uint32_t chnls = AUDIO_CHANNELS;
    const ALCsizei bufSize = (frameDuration * sampleRate * 4) / 1000 * chnls;
    if (inDevDescr.isEmpty())
    {
        const ALchar *pDeviceList = Audio::inDeviceNames();
        if (pDeviceList)
            inDevDescr = QString::fromUtf8(pDeviceList, strlen(pDeviceList));
    }

    if (!inDevDescr.isEmpty())
        alInDev = alcCaptureOpenDevice(inDevDescr.toUtf8().constData(),
                                       sampleRate, stereoFlag, bufSize);

    // Restart the capture if necessary
    if (!alInDev)
    {
        qWarning() << "Failed to initialize audio input device:" << inDevDescr;
        return false;
    }

    d->setInputGain(Settings::getInstance().getAudioInGain());

    qDebug() << "Opened audio input" << inDevDescr;
    alcCaptureStart(alInDev);

    return true;
}

/**
@internal

Open an audio output device
*/
bool Audio::initOutput(QString outDevDescr)
{
    qDebug() << "Opening audio output" << outDevDescr;
    outSources.clear();

    outputInitialized = false;
    if (outDevDescr == "none")
        return false;

    assert(!alOutDev);

    if (outDevDescr.isEmpty())
    {
        // default to the first available audio device.
        const ALchar *pDeviceList = Audio::outDeviceNames();
        if (pDeviceList)
            outDevDescr = QString::fromUtf8(pDeviceList, strlen(pDeviceList));
    }

    if (!outDevDescr.isEmpty())
        alOutDev = alcOpenDevice(outDevDescr.toUtf8().constData());

    if (!alOutDev)
    {
        qWarning() << "Cannot open output audio device" << outDevDescr;
        return false;
    }

    qDebug() << "Opened audio output" << outDevDescr;
    alOutContext = alcCreateContext(alOutDev, nullptr);
    checkAlcError(alOutDev);

    if (!alcMakeContextCurrent(alOutContext))
    {
        qWarning() << "Cannot create output audio context";
        return false;
    }

    alGenSources(1, &alMainSource);
    checkAlError();

    // init master volume
    alListenerf(AL_GAIN, Settings::getInstance().getOutVolume() * 0.01f);
    checkAlError();

    Core* core = Core::getInstance();
    if (core)
    {
        // reset each call's audio source
        core->getAv()->invalidateCallSources();
    }

    outputInitialized = true;
    return true;
}

/**
Play a 44100Hz mono 16bit PCM sound from a file
*/
void Audio::playMono16Sound(const QString& path)
{
    QFile sndFile(path);
    sndFile.open(QIODevice::ReadOnly);
    playMono16Sound(QByteArray(sndFile.readAll()));
}

/**
Play a 44100Hz mono 16bit PCM sound
*/
void Audio::playMono16Sound(const QByteArray& data)
{
    QMutexLocker locker(&audioLock);

    if (!autoInitOutput())
        return;

    if (!alMainBuffer)
        alGenBuffers(1, &alMainBuffer);

    ALint state;
    alGetSourcei(alMainSource, AL_SOURCE_STATE, &state);
    if (state == AL_PLAYING)
    {
        alSourceStop(alMainSource);
        alSourcei(alMainSource, AL_BUFFER, AL_NONE);
    }

    alBufferData(alMainBuffer, AL_FORMAT_MONO16, data.constData(), data.size(), 44100);
    alSourcei(alMainSource, AL_BUFFER, static_cast<ALint>(alMainBuffer));
    alSourcePlay(alMainSource);

    int durationMs = data.size() * 1000 / 2 / 44100;
    playMono16Timer.start(durationMs + 50);
}

void Audio::playAudioBuffer(ALuint alSource, const int16_t *data, int samples, unsigned channels, int sampleRate)
{
    assert(channels == 1 || channels == 2);
    QMutexLocker locker(&audioLock);

    if (!(alOutDev && outputInitialized))
        return;

    ALuint bufid;
    ALint processed = 0, queued = 16;
    alGetSourcei(alSource, AL_BUFFERS_PROCESSED, &processed);
    alGetSourcei(alSource, AL_BUFFERS_QUEUED, &queued);
    alSourcei(alSource, AL_LOOPING, AL_FALSE);

    if (processed)
    {
        ALuint bufids[processed];
        alSourceUnqueueBuffers(alSource, processed, bufids);
        alDeleteBuffers(processed - 1, bufids + 1);
        bufid = bufids[0];
    }
    else if (queued < 16)
    {
        alGenBuffers(1, &bufid);
    }
    else
    {
        return;
    }

    alBufferData(bufid, (channels == 1) ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16, data,
                    samples * 2 * channels, sampleRate);
    alSourceQueueBuffers(alSource, 1, &bufid);

    ALint state;
    alGetSourcei(alSource, AL_SOURCE_STATE, &state);
    if (state != AL_PLAYING)
        alSourcePlay(alSource);
}

/**
@internal

Close active audio input device.
*/
void Audio::cleanupInput()
{
    if (!alInDev)
        return;

    qDebug() << "Closing audio input";
    alcCaptureStop(alInDev);
    if (alcCaptureCloseDevice(alInDev) == ALC_TRUE)
        alInDev = nullptr;
    else
        qWarning() << "Failed to close input";
}

/**
@internal

Close active audio output device
*/
void Audio::cleanupOutput()
{
    outputInitialized = false;

    if (alOutDev)
    {
        alSourcei(alMainSource, AL_LOOPING, AL_FALSE);
        alSourceStop(alMainSource);
        alDeleteSources(1, &alMainSource);

        if (alMainBuffer)
        {
            alDeleteBuffers(1, &alMainBuffer);
            alMainBuffer = 0;
        }

        if (!alcMakeContextCurrent(nullptr))
            qWarning("Failed to clear audio context.");

        alcDestroyContext(alOutContext);
        alOutContext = nullptr;

        qDebug() << "Closing audio output";
        if (alcCloseDevice(alOutDev))
            alOutDev = nullptr;
        else
            qWarning("Failed to close output.");
    }
}

void Audio::playMono16SoundCleanup()
{
    QMutexLocker locker(&audioLock);

    ALint state;
    alGetSourcei(alMainSource, AL_SOURCE_STATE, &state);
    if (state == AL_STOPPED)
    {
        alSourcei(alMainSource, AL_BUFFER, AL_NONE);
        alDeleteBuffers(1, &alMainBuffer);
        alMainBuffer = 0;
    }
}

void Audio::doCapture()
{
    QMutexLocker lock(&audioLock);

    if (!alInDev || !inSubscriptions)
        return;

    ALint curSamples = 0;
    alcGetIntegerv(alInDev, ALC_CAPTURE_SAMPLES, sizeof(curSamples), &curSamples);
    if (curSamples < AUDIO_FRAME_SAMPLE_COUNT)
        return;

    int16_t buf[AUDIO_FRAME_SAMPLE_COUNT * AUDIO_CHANNELS];
    alcCaptureSamples(alInDev, buf, AUDIO_FRAME_SAMPLE_COUNT);

#ifdef QTOX_FILTER_AUDIO
    if (Settings::getInstance().getFilterAudio())
    {
#ifdef ALC_LOOPBACK_CAPTURE_SAMPLES
        // compatibility with older versions of OpenAL
        getEchoesToFilter(filterer, AUDIO_FRAME_SAMPLE_COUNT * AUDIO_CHANNELS);
#endif
        filterer.filterAudio(buf, AUDIO_FRAME_SAMPLE_COUNT * AUDIO_CHANNELS);
    }
#endif

    for (quint32 i = 0; i < AUDIO_FRAME_SAMPLE_COUNT * AUDIO_CHANNELS; ++i)
    {
        // gain amplification with clipping to 16-bit boundaries
        int ampPCM = qBound<int>(std::numeric_limits<int16_t>::min(),
                                 qRound(buf[i] * d->inputGainFactor()),
                                 std::numeric_limits<int16_t>::max());

        buf[i] = static_cast<int16_t>(ampPCM);
    }

    emit frameAvailable(buf, AUDIO_FRAME_SAMPLE_COUNT, AUDIO_CHANNELS, AUDIO_SAMPLE_RATE);
}

/**
Returns true if the input device is open and suscribed to
*/
bool Audio::isInputReady() const
{
    QMutexLocker locker(&audioLock);
    return alInDev && inSubscriptions;
}

/**
Returns true if the output device is open
*/
bool Audio::isOutputReady() const
{
    QMutexLocker locker(&audioLock);
    return alOutDev && outputInitialized;
}

const char* Audio::outDeviceNames()
{
    const char* pDeviceList;
    if (alcIsExtensionPresent(NULL, "ALC_ENUMERATE_ALL_EXT") != AL_FALSE)
        pDeviceList = alcGetString(NULL, ALC_ALL_DEVICES_SPECIFIER);
    else
        pDeviceList = alcGetString(NULL, ALC_DEVICE_SPECIFIER);

    return pDeviceList;
}

const char* Audio::inDeviceNames()
{
    return alcGetString(NULL, ALC_CAPTURE_DEVICE_SPECIFIER);
}

void Audio::subscribeOutput(ALuint& sid)
{
    QMutexLocker locker(&audioLock);

    if (!autoInitOutput())
    {
        qWarning("Failed to subscribe to audio output device.");
        return;
    }

    if (!alcMakeContextCurrent(alOutContext))
    {
        qWarning("Failed to activate output context.");
        return;
    }

    alGenSources(1, &sid);
    assert(sid);
    outSources << sid;

    qDebug() << "Audio source" << sid << "created. Sources active:"
             << outSources.size();
}

void Audio::unsubscribeOutput(ALuint &sid)
{
    QMutexLocker locker(&audioLock);

    outSources.removeAll(sid);

    if (sid)
    {
        if (alIsSource(sid))
        {
            alDeleteSources(1, &sid);
            qDebug() << "Audio source" << sid << "deleted. Sources active:"
                     << outSources.size();
        } else {
            qWarning() << "Trying to delete invalid audio source" << sid;
        }

        sid = 0;
    }

    if (outSources.isEmpty())
        cleanupOutput();
}

void Audio::startLoop()
{
    QMutexLocker locker(&audioLock);
    alSourcei(alMainSource, AL_LOOPING, AL_TRUE);
}

void Audio::stopLoop()
{
    QMutexLocker locker(&audioLock);
    alSourcei(alMainSource, AL_LOOPING, AL_FALSE);
    alSourceStop(alMainSource);
}

#if defined(QTOX_FILTER_AUDIO) && defined(ALC_LOOPBACK_CAPTURE_SAMPLES)
void Audio::getEchoesToFilter(AudioFilterer* filterer, int samples)
{
    ALint samples;
    alcGetIntegerv(&alOutDev, ALC_LOOPBACK_CAPTURE_SAMPLES, sizeof(samples), &samples);
    if (samples >= samples)
    {
        int16_t buf[samples];
        alcCaptureSamplesLoopback(&alOutDev, buf, samples);
        filterer->passAudioOutput(buf, samples);
        filterer->setEchoDelayMs(5); // This 5ms is configurable I believe
    }
}
#endif
