#pragma once

#include <QString>
#include <QStringList>
#include <atomic>

namespace Mandarina::Audio {

enum class Status { Success, Failure, Cancelled };

struct Outcome {
    Status status = Status::Failure;
    QString error;
};

// Extension classification is deliberately cheap; convert() validates the audio
// stream itself before starting an encoder.
bool isSource(const QString &path);

// Only formats whose encoder and muxer are present in this FFmpeg installation.
// The small capability query is cached for this process.
QStringList outputFormats();

// The caller supplies a new file path inside its own private temporary directory
// and publishes the result only after Success. This function never overwrites,
// renames, removes or publishes any file, including on cancellation or failure.
Outcome convert(const QString &input, const QString &format,
                const QString &privateOutputPath, std::atomic_bool &cancel);

} // namespace Mandarina::Audio
