#pragma once

#include <QObject>
#include <memory>

// A short, local hover cue. Audio setup and playback stay off the UI thread;
// rapid requests are discarded rather than accumulated or played together.
class WheelSound final : public QObject
{
    Q_OBJECT
public:
    explicit WheelSound(QObject *parent = nullptr);
    ~WheelSound() override;

    Q_INVOKABLE void play();

private:
    struct Private;
    std::unique_ptr<Private> d;
};
