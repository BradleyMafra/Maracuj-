#include "wheelsound.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QMutex>
#include <QThread>
#include <QWaitCondition>
#include <QtEndian>
#include <cmath>
#include <cstdint>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
#else
#include <QDir>
#include <QLibrary>
#include <QTemporaryFile>
#endif

namespace {

constexpr int SampleRate = 24000;
constexpr int SampleCount = SampleRate * 48 / 1000;
constexpr qint64 MinimumIntervalMs = 65;
constexpr qint64 RetryIntervalMs = 5000;

QByteArray createWave()
{
    // A softly struck, consonant wooden tone with a smooth attack and tail.
    // Generating the tiny PCM sample locally avoids an audio asset dependency.
    QByteArray wave(44 + SampleCount * 2, '\0');
    auto put16 = [&wave](int offset, quint16 value) {
        qToLittleEndian(value, wave.data() + offset);
    };
    auto put32 = [&wave](int offset, quint32 value) {
        qToLittleEndian(value, wave.data() + offset);
    };
    wave.replace(0, 4, "RIFF");
    put32(4, static_cast<quint32>(wave.size() - 8));
    wave.replace(8, 8, "WAVEfmt ");
    put32(16, 16);
    put16(20, 1); // PCM
    put16(22, 1); // Mono
    put32(24, SampleRate);
    put32(28, SampleRate * 2);
    put16(32, 2);
    put16(34, 16);
    wave.replace(36, 4, "data");
    put32(40, SampleCount * 2);

    constexpr double Pi = 3.14159265358979323846;
    for (int i = 0; i < SampleCount; ++i) {
        const double t = static_cast<double>(i) / SampleRate;
        const double remaining = 1.0 - static_cast<double>(i) / (SampleCount - 1);
        const double envelope = (1.0 - std::exp(-t * 900.0))
            * std::exp(-t * 75.0) * remaining * remaining;
        const double tone = std::sin(2.0 * Pi * 880.0 * t)
            + 0.3 * std::sin(2.0 * Pi * 1320.0 * t) * std::exp(-t * 35.0)
            + 0.12 * std::sin(2.0 * Pi * 2640.0 * t) * std::exp(-t * 90.0);
        const auto sample = static_cast<qint16>(std::lround(32767.0 * 0.16 * envelope * tone));
        put16(44 + i * 2, static_cast<quint16>(sample));
    }
    return wave;
}

#ifdef Q_OS_WIN
class AudioBackend
{
public:
    bool initialize() { return true; }
    bool play()
    {
        const bool accepted = PlaySoundW(reinterpret_cast<LPCWSTR>(wave.constData()), nullptr,
                                        SND_ASYNC | SND_MEMORY | SND_NODEFAULT) != FALSE;
        // A failed replacement must not release a buffer still referenced by
        // an earlier asynchronous cue. Keep shutdown cancellation armed.
        played = played || accepted;
        return accepted;
    }
    ~AudioBackend()
    {
        // The in-memory WAV must remain alive until asynchronous playback ends.
        if (played)
            PlaySoundW(nullptr, nullptr, 0);
    }

private:
    const QByteArray wave = createWave();
    bool played = false;
};
#else
struct ca_context;

class AudioBackend
{
public:
    bool initialize()
    {
        if (!library.load())
            return false;
        create = reinterpret_cast<Create>(library.resolve("ca_context_create"));
        destroy = reinterpret_cast<Destroy>(library.resolve("ca_context_destroy"));
        changeProps = reinterpret_cast<ChangeProps>(library.resolve("ca_context_change_props"));
        playSound = reinterpret_cast<Play>(library.resolve("ca_context_play"));
        cancel = reinterpret_cast<Cancel>(library.resolve("ca_context_cancel"));
        if (!create || !destroy || !changeProps || !playSound || !cancel)
            return false;

        const QByteArray wave = createWave();
        if (!sample.open() || sample.write(wave) != wave.size() || !sample.flush())
            return false;
        filename = sample.fileName().toUtf8();
        sample.close();
        if (create(&context) < 0)
            return false;
        return changeProps(context, "application.name", "Maracujá",
                           "application.id", "org.winux.Mandarina",
                           "media.role", "event", static_cast<const char *>(nullptr)) >= 0;
    }
    bool play()
    {
        // A fixed event ID cancels any stale backend playback before the next
        // tick. ca_context_play is asynchronous and never queues hover cues.
        cancel(context, EventId);
        return playSound(context, EventId, "media.filename", filename.constData(),
                         "canberra.cache-control", "permanent",
                         static_cast<const char *>(nullptr)) >= 0;
    }
    ~AudioBackend()
    {
        if (context) {
            cancel(context, EventId);
            destroy(context);
        }
    }

private:
    using Create = int (*)(ca_context **);
    using Destroy = int (*)(ca_context *);
    using ChangeProps = int (*)(ca_context *, ...);
    using Play = int (*)(ca_context *, std::uint32_t, ...);
    using Cancel = int (*)(ca_context *, std::uint32_t);
    static constexpr std::uint32_t EventId = 1;

    QLibrary library{QStringLiteral("libcanberra.so.0")};
    QTemporaryFile sample{QDir::tempPath() + QStringLiteral("/maracuja-wheel-XXXXXX.wav")};
    QByteArray filename;
    ca_context *context = nullptr;
    Create create = nullptr;
    Destroy destroy = nullptr;
    ChangeProps changeProps = nullptr;
    Play playSound = nullptr;
    Cancel cancel = nullptr;
};
#endif

class AudioThread final : public QThread
{
public:
    AudioThread() { setObjectName(QStringLiteral("MaracujaWheelAudio")); }
    ~AudioThread() override
    {
        {
            QMutexLocker lock(&mutex);
            stopping = true;
            wake.wakeOne();
        }
        wait();
    }
    void request()
    {
        QMutexLocker lock(&mutex);
        if (stopping || pending || busy
            || (retryTimer.isValid() && retryTimer.elapsed() < RetryIntervalMs)
            || (lastRequest.isValid() && lastRequest.elapsed() < MinimumIntervalMs))
            return;
        lastRequest.start();
        pending = true;
        if (!isRunning())
            start();
        else
            wake.wakeOne();
    }

protected:
    void run() override
    {
        // Loading the optional Linux backend and connecting to the audio
        // server happen here too, so the first hover cannot block the UI.
        std::unique_ptr<AudioBackend> backend;
        QMutexLocker lock(&mutex);
        while (!stopping) {
            if (!pending) {
                wake.wait(&mutex);
                continue;
            }
            pending = false;
            busy = true;
            lock.unlock();
            if (!backend) {
                backend = std::make_unique<AudioBackend>();
                if (!backend->initialize())
                    backend.reset();
            }
            const bool played = backend && backend->play();
            if (!played)
                backend.reset();
            lock.relock();
            busy = false;
            if (played)
                retryTimer.invalidate();
            else
                // A temporarily disconnected output can recover on a later
                // hover without restarting the app or flooding the backend.
                retryTimer.start();
        }
    }

private:
    QMutex mutex;
    QWaitCondition wake;
    QElapsedTimer lastRequest;
    QElapsedTimer retryTimer;
    bool stopping = false;
    bool pending = false;
    bool busy = false;
};

} // namespace

struct WheelSound::Private
{
    AudioThread thread;
};

WheelSound::WheelSound(QObject *parent) : QObject(parent), d(std::make_unique<Private>()) {}

WheelSound::~WheelSound() = default;

void WheelSound::play()
{
    d->thread.request();
}
