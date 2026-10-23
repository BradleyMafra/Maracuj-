#pragma once

#include <QString>
#include <QStringList>
#include <atomic>
#include <functional>

namespace Mandarina::Documents {

struct Outcome {
    enum Status { Success, Failure, Cancelled };
    Status status = Failure;
    QString error;
};

bool isSource(const QString &path);
QStringList outputFormats();
QStringList pdfOutputFormats();
QStringList imageOcrFormats();
using Progress = std::function<void(int, int, const QString &)>;
Outcome convert(const QString &input, const QString &format,
                const QString &privateOutputPath, std::atomic_bool &cancel,
                const Progress &progress = {});
Outcome convertImage(const QString &preparedPng, const QString &format,
                     const QString &privateOutputPath, std::atomic_bool &cancel,
                     const Progress &progress = {});

} // namespace Mandarina::Documents
