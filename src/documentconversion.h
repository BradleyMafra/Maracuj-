#pragma once

#include <QString>
#include <QStringList>
#include <atomic>
#include <functional>
#include <memory>

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
// Discovery can run on a background thread; cached reads never wait for tools.
void prepareCapabilities();
bool capabilitiesReady();
QStringList cachedOutputFormats();
QStringList cachedPdfOutputFormats();
QStringList cachedImageOcrFormats();
using Progress = std::function<void(int, int, const QString &)>;
class Session {
public:
    Session();
    ~Session();
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;
    Outcome run(const QStringList &arguments, std::atomic_bool &cancel,
                int timeoutMs, const Progress &progress);
private:
    struct State;
    std::unique_ptr<State> state;
};
Outcome convert(const QString &input, const QString &format,
                const QString &privateOutputPath, std::atomic_bool &cancel,
                const Progress &progress = {}, Session *session = nullptr);
Outcome convertImage(const QString &preparedPng, const QString &format,
                     const QString &privateOutputPath, std::atomic_bool &cancel,
                     const Progress &progress = {}, Session *session = nullptr);

} // namespace Mandarina::Documents
