#include "wheelsound.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QDebug>
#include <stdexcept>

extern "C" {
void fakeAudioBlockInitialization(int block);
int fakeAudioCount(int kind);
int fakeAudioWait(int kind, int count, int timeoutMs);
int fakeAudioErrors();
int fakeAudioFileExists();
void fakeAudioFailNextPlay();
}

namespace {
void require(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}

struct ReleaseInitialization
{
    ~ReleaseInitialization() { fakeAudioBlockInitialization(0); }
};
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    try {
        {
            WheelSound sound;
            ReleaseInitialization release;
            require(fakeAudioCount(0) == 0 && fakeAudioCount(1) == 0,
                    "Constructing the sound engine must be silent and lazy");

            fakeAudioBlockInitialization(1);
            QElapsedTimer timer;
            timer.start();
            sound.play();
            require(timer.elapsed() < 100, "Audio initialization blocked the caller");
            require(fakeAudioWait(0, 1, 1500), "The worker did not initialize the fake backend");
            for (int i = 0; i < 2000; ++i)
                sound.play();
            require(fakeAudioCount(1) == 0, "Playback escaped the blocked worker");
            fakeAudioBlockInitialization(0);
            require(fakeAudioWait(1, 1, 1500), "The first cue did not play");
            require(fakeAudioFileExists(), "The asynchronous sample lost its file");

            // Wait beyond the real 65ms gate once, then ensure an ordinary
            // later selection can play and another rapid burst adds no queue.
            QThread::msleep(80);
            require(fakeAudioCount(1) == 1, "The initial burst accumulated queued cues");
            for (int i = 0; i < 2000; ++i)
                sound.play();
            require(fakeAudioWait(1, 2, 1500), "A later cue was incorrectly suppressed");
            QThread::msleep(80);
            require(fakeAudioCount(1) == 2, "Rapid selection changes accumulated cues");

            fakeAudioFailNextPlay();
            sound.play();
            require(fakeAudioWait(1, 3, 1500) && fakeAudioWait(2, 1, 1500),
                    "A failed playback did not release its backend");
            timer.restart();
            for (int i = 0; i < 2000; ++i)
                sound.play();
            require(fakeAudioCount(0) == 1, "Failure retries ignored the cooldown");

            // Focused recovery check: the actual 5s backoff must expire so an
            // output-device interruption does not require an app restart.
            while (fakeAudioCount(1) < 4 && timer.elapsed() < 7000) {
                sound.play();
                QThread::msleep(20);
            }
            require(fakeAudioCount(1) == 4 && fakeAudioCount(0) == 2,
                    "The engine did not recreate the backend after a transient failure");
            require(timer.elapsed() >= 4900, "A failed backend retried too soon");
            require(fakeAudioErrors() == 0, "The sample format, amplitude, privacy, or lifetime was invalid");
        }
        require(fakeAudioCount(2) == 2, "Audio backend destruction was incomplete");
        require(fakeAudioCount(3) >= 6, "Playback or shutdown omitted cancellation");
        require(!fakeAudioFileExists(), "The scoped temporary WAV was not removed");
        require(fakeAudioErrors() == 0, "Cleanup removed the sample before backend destruction");
    } catch (const std::exception &error) {
        qCritical() << error.what();
        return 1;
    }
    qInfo() << "Silent audio backend checks passed";
    return 0;
}
