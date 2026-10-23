#pragma once

#include <QImage>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <atomic>

namespace Mandarina::Photo {
struct Loaded {
    QImage image;
    QSize originalSize; // Dimensions after applying EXIF orientation.
    QString error;
};

struct ExportResult {
    QString output;
    QString error;
    bool cancelled = false;
};

// These functions are intended for workers. supportsPath only inspects the
// file/header and is also suitable for filtering the gesture's offered tools.
bool supportsPath(const QString &path);
Loaded loadImage(const QString &path, int maxEdge, std::atomic_bool &cancel);
QImage apply(const QImage &source, const QVariantMap &adjustments,
             std::atomic_bool &cancel, QString &error);
ExportResult exportCopy(const QString &path, const QString &outputDirectory,
                        const QString &format, int quality,
                        const QVariantMap &adjustments, std::atomic_bool &cancel);
QStringList outputFormats();
} // namespace Mandarina::Photo
