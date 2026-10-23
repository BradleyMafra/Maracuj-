#pragma once

#include <QString>
#include <QStringList>
#include <atomic>
#include <functional>

namespace Mandarina {

struct Request {
    QStringList inputFiles;
    QString format;
    QString outputDir;
    int quality = 90;
    int dpi = 150;
};

struct Result {
    QStringList outputs;
    QStringList errors;
    bool cancelled = false;
};

using Progress = std::function<void(int, int, const QString &)>;

Result convert(const Request &, std::atomic_bool &, const Progress &);
QString sourceKind(const QString &path);
QStringList imageOutputFormats();
QStringList outputFormats(const QString &kind);

} // namespace Mandarina
