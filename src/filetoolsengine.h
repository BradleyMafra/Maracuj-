#pragma once
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <atomic>
#include <functional>

namespace Mandarina::FileTools {
struct Request {
    QStringList paths;
    QString action; // compress or metadata
    QVariantMap options;
    QString outputDirectory; // Empty: alongside each source.
};
struct Outcome {
    QStringList outputs;
    QStringList errors;
    QVariantList metadata;
    QVariantList details;
    bool cancelled = false;
};
using Progress = std::function<void(int, int, const QString &)>;
Outcome inspect(const QString &path, std::atomic_bool &cancel);
Outcome run(const Request &request, std::atomic_bool &cancel, const Progress &progress = {});
} // namespace Mandarina::FileTools
