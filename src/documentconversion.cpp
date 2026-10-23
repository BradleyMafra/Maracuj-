#include "documentconversion.h"
#include "runtimepaths.h"
#include "ownedprocess.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QMutex>
#include <QMutexLocker>

#ifdef Q_OS_UNIX
#include <csignal>
#include <unistd.h>
#endif

namespace Mandarina::Documents {
namespace {

struct Runtime {
    QString python;
    QString soffice;
    QString helper;
    QString tessdata;
};

Runtime runtime()
{
    return {RuntimePaths::python(), RuntimePaths::soffice(),
            RuntimePaths::helper(QStringLiteral("document_convert.py")), RuntimePaths::tessdata()};
}

Outcome run(const QStringList &arguments, std::atomic_bool &cancel,
            QByteArray &output, int timeoutMs, const Progress &progress = {})
{
    if (cancel.load()) return {Outcome::Cancelled, {}};
    const Runtime config = runtime();
    if (!QFileInfo(config.python).isExecutable() || !QFileInfo(config.helper).isFile())
        return {Outcome::Failure, QStringLiteral("O componente local de documentos não está disponível.")};
    OwnedProcess process;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
#ifndef Q_OS_WIN
    // Standalone Python carries its own OpenSSL. Qt/FFmpeg's portable library
    // path must not override it; CLI/OCR tools have their own relative RPATH.
    environment.remove(QStringLiteral("LD_LIBRARY_PATH"));
#endif
    environment.insert(QStringLiteral("PYTHONDONTWRITEBYTECODE"), QStringLiteral("1"));
    environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C.UTF-8"));
    environment.insert(QStringLiteral("OMP_THREAD_LIMIT"), QStringLiteral("1"));
    environment.insert(QStringLiteral("OMP_NUM_THREADS"), QStringLiteral("1"));
    process.setProcessEnvironment(environment);
    process.setProgram(config.python);
    process.setArguments(QStringList{QStringLiteral("-I"), config.helper,
        QStringLiteral("--runtime-root"), RuntimePaths::root(),
        QStringLiteral("--tessdata"), config.tessdata} + arguments);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    if (!process.startOwned())
        return {Outcome::Failure, QStringLiteral("Não foi possível iniciar a conversão local de documentos.")};
    QElapsedTimer timer;
    timer.start();
    Outcome outcome{Outcome::Success, {}};
    QByteArray pending;
    const auto drain = [&]() {
        pending.append(process.readAllStandardOutput());
        if (pending.size() > 64 * 1024) return false;
        qsizetype newline;
        while ((newline = pending.indexOf('\n')) >= 0) {
            const auto line = pending.left(newline).trimmed();
            pending.remove(0, newline + 1);
            if (line.isEmpty()) continue;
            const auto event = QJsonDocument::fromJson(line).object();
            if (event.isEmpty()) return false;
            if (event.value(QStringLiteral("type")).toString() == QStringLiteral("progress")) {
                const int done = event.value(QStringLiteral("done")).toInt(-1);
                const int total = event.value(QStringLiteral("total")).toInt(-1);
                const auto phase = event.value(QStringLiteral("phase")).toString();
                if (done < 0 || total < 1 || total > 200 || done > total
                    || (phase != QStringLiteral("text") && phase != QStringLiteral("ocr"))) return false;
                if (progress) progress(done, total, phase);
            } else {
                if (!output.isEmpty()) return false;
                output = line;
            }
        }
        return true;
    };
    while (process.state() != QProcess::NotRunning) {
        process.waitForFinished(75);
        const bool validOutput = drain();
        // External programs can include private paths/content in diagnostics: discard them.
        process.readAllStandardError();
        if (cancel.load()) {
            outcome = {Outcome::Cancelled, {}};
            break;
        }
        if (!validOutput || timer.elapsed() > timeoutMs) {
            outcome = {Outcome::Failure, QStringLiteral("A conversão excedeu o limite de tempo ou de recursos.")};
            break;
        }
    }
    process.stopOwned();
    // A failed tool must not leave a detached office worker/converter behind.
    process.finishOwnedTree();
    if (outcome.status != Outcome::Success) return outcome;
    if (!drain() || !pending.trimmed().isEmpty())
        return {Outcome::Failure, QStringLiteral("Resposta inválida do conversor local de documentos.")};
    process.readAllStandardError();
    if (output.size() > 64 * 1024)
        return {Outcome::Failure, QStringLiteral("Resposta inválida do conversor local de documentos.")};
    const auto response = QJsonDocument::fromJson(output.trimmed()).object();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0
        || !response.value(QStringLiteral("ok")).toBool()) {
        QString error = response.value(QStringLiteral("error")).toString();
        if (error.isEmpty() || error.size() > 512)
            error = QStringLiteral("Não foi possível converter este documento.");
        return {Outcome::Failure, error};
    }
    return outcome;
}

struct CapabilityCache {
    QMutex discovery;
    QMutex data;
    QJsonObject value;
    bool initialized = false;
};

CapabilityCache &capabilityCache()
{
    static CapabilityCache cache;
    return cache;
}

QJsonObject capabilities(bool discover = true)
{
    auto &cache = capabilityCache();
    {
        QMutexLocker lock(&cache.data);
        if (cache.initialized || !discover) return cache.value;
    }
    QMutexLocker discovery(&cache.discovery);
    {
        QMutexLocker lock(&cache.data);
        if (cache.initialized) return cache.value;
    }
    QJsonObject value;
    {
        QByteArray output;
        std::atomic_bool cancel{false};
        const auto config = runtime();
        const auto result = run({QStringLiteral("--capabilities"), QStringLiteral("--soffice"),
                                 config.soffice}, cancel, output, 5000);
        if (result.status == Outcome::Success) value = QJsonDocument::fromJson(output.trimmed()).object();
    }
    QMutexLocker lock(&cache.data);
    cache.value = value;
    cache.initialized = true;
    return cache.value;
}

QStringList formats(const QString &key, bool discover = true)
{
    QStringList result;
    const auto values = capabilities(discover).value(key).toArray();
    for (const auto &value : values) {
        const QString format = value.toString();
        if (!format.isEmpty()) result.append(format);
    }
    return result;
}

} // namespace

struct Session::State {
#ifdef Q_OS_WIN
    std::unique_ptr<OwnedProcess> process;
    int completed = 0;
    int requestId = 0;
    void stop(bool graceful = false)
    {
        if (!process) return;
        if (graceful && process->state() != QProcess::NotRunning) {
            process->closeWriteChannel();
            process->waitForFinished(500);
        }
        process->stopOwned();
        process->finishOwnedTree();
        process.reset();
        completed = 0;
    }
    ~State() { stop(true); }
#endif
};

Session::Session() : state(std::make_unique<State>()) {}
Session::~Session() = default;

Outcome Session::run(const QStringList &arguments, std::atomic_bool &cancel,
                     int timeoutMs, const Progress &progress)
{
#ifndef Q_OS_WIN
    QByteArray output;
    return Documents::run(arguments, cancel, output, timeoutMs, progress);
#else
    if (cancel.load()) return {Outcome::Cancelled, {}};
    if (state->process && (state->completed >= 32 || state->process->state() == QProcess::NotRunning))
        state->stop(true);
    if (!state->process) {
        const auto config = runtime();
        if (!QFileInfo(config.python).isExecutable() || !QFileInfo(config.helper).isFile())
            return {Outcome::Failure, QStringLiteral("O componente local de documentos não está disponível.")};
        state->process = std::make_unique<OwnedProcess>();
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("PYTHONDONTWRITEBYTECODE"), QStringLiteral("1"));
        environment.insert(QStringLiteral("OMP_THREAD_LIMIT"), QStringLiteral("1"));
        environment.insert(QStringLiteral("OMP_NUM_THREADS"), QStringLiteral("1"));
        state->process->setProcessEnvironment(environment);
        state->process->setProgram(config.python);
        state->process->setArguments({QStringLiteral("-I"), config.helper, QStringLiteral("--serve"),
            QStringLiteral("--runtime-root"), RuntimePaths::root(), QStringLiteral("--tessdata"), config.tessdata,
            QStringLiteral("--soffice"), config.soffice});
        state->process->setProcessChannelMode(QProcess::SeparateChannels);
        if (!state->process->startOwned(QIODevice::ReadWrite)) {
            state->stop();
            return {Outcome::Failure, QStringLiteral("Não foi possível iniciar a conversão local de documentos.")};
        }
    }
    auto &process = *state->process;
    const int requestId = ++state->requestId;
    QJsonObject request{{QStringLiteral("id"), requestId}, {QStringLiteral("image_ocr"), false}};
    for (int index = 0; index < arguments.size(); ++index) {
        const auto &argument = arguments.at(index);
        if (argument == QStringLiteral("--image-ocr")) request.insert(QStringLiteral("image_ocr"), true);
        else if (argument == QStringLiteral("--input") || argument == QStringLiteral("--output")
                 || argument == QStringLiteral("--format")) {
            if (++index >= arguments.size()) { state->stop(); return {Outcome::Failure, QStringLiteral("Parâmetros de conversão inválidos.")}; }
            request.insert(argument.mid(2), arguments.at(index));
        }
    }
    const auto encoded = QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n';
    if (encoded.size() > 64 * 1024 || process.write(encoded) != encoded.size()) {
        state->stop();
        return {Outcome::Failure, QStringLiteral("Não foi possível enviar a conversão ao componente local.")};
    }
    QElapsedTimer timer;
    timer.start();
    QByteArray pending;
    QJsonObject response;
    bool valid = true;
    while (response.isEmpty() && valid && !cancel.load() && timer.elapsed() <= timeoutMs) {
        process.waitForReadyRead(75);
        pending.append(process.readAllStandardOutput());
        // Never retain/log private tool diagnostics.
        process.readAllStandardError();
        if (pending.size() > 64 * 1024) { valid = false; break; }
        qsizetype newline;
        while ((newline = pending.indexOf('\n')) >= 0) {
            const auto line = pending.left(newline).trimmed();
            pending.remove(0, newline + 1);
            const auto event = QJsonDocument::fromJson(line).object();
            if (event.value(QStringLiteral("id")).toInt(-1) != requestId || !response.isEmpty()) { valid = false; break; }
            const auto type = event.value(QStringLiteral("type")).toString();
            if (type == QStringLiteral("progress")) {
                const int done = event.value(QStringLiteral("done")).toInt(-1);
                const int total = event.value(QStringLiteral("total")).toInt(-1);
                const auto phase = event.value(QStringLiteral("phase")).toString();
                if (done < 0 || total < 1 || total > 200 || done > total
                    || (phase != QStringLiteral("text") && phase != QStringLiteral("ocr"))) { valid = false; break; }
                if (progress) progress(done, total, phase);
            } else if (type == QStringLiteral("result") && event.value(QStringLiteral("ok")).isBool()) response = event;
            else { valid = false; break; }
        }
        if (process.state() == QProcess::NotRunning && response.isEmpty()) break;
    }
    if (cancel.load()) { state->stop(); return {Outcome::Cancelled, {}}; }
    if (!valid || response.isEmpty() || !pending.isEmpty() || timer.elapsed() > timeoutMs
        || (process.state() == QProcess::NotRunning
            && (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0))) {
        // Do not retry an in-flight request: it may already have written output.
        state->stop();
        return {Outcome::Failure, QStringLiteral("A conversão local foi interrompida ou excedeu o limite de tempo ou de recursos.")};
    }
    ++state->completed;
    if (!response.value(QStringLiteral("ok")).toBool()) {
        auto error = response.value(QStringLiteral("error")).toString();
        if (error.isEmpty() || error.size() > 512) error = QStringLiteral("Não foi possível converter este documento.");
        state->stop(); // An unsuccessful tool must not leave descendants alive.
        return {Outcome::Failure, error};
    }
    return {Outcome::Success, {}};
#endif
}

bool isSource(const QString &path)
{
    static const QStringList extensions{QStringLiteral("txt"), QStringLiteral("md"),
        QStringLiteral("markdown"), QStringLiteral("html"), QStringLiteral("htm"),
        QStringLiteral("docx"), QStringLiteral("odt"), QStringLiteral("rtf")};
    return extensions.contains(QFileInfo(path).suffix().toLower());
}

QStringList outputFormats() { return formats(QStringLiteral("documents")); }
QStringList pdfOutputFormats() { return formats(QStringLiteral("pdf")); }
QStringList imageOcrFormats() { return formats(QStringLiteral("image_ocr")); }
void prepareCapabilities() { capabilities(); }
bool capabilitiesReady()
{
    auto &cache = capabilityCache();
    QMutexLocker lock(&cache.data);
    return cache.initialized;
}
QStringList cachedOutputFormats() { return formats(QStringLiteral("documents"), false); }
QStringList cachedPdfOutputFormats() { return formats(QStringLiteral("pdf"), false); }
QStringList cachedImageOcrFormats() { return formats(QStringLiteral("image_ocr"), false); }

Outcome convert(const QString &input, const QString &format,
                const QString &privateOutputPath, std::atomic_bool &cancel, const Progress &progress, Session *session)
{
    if (cancel.load()) return {Outcome::Cancelled, {}};
    const QFileInfo source(input);
    const QString normalized = format.toLower();
    const bool document = isSource(input);
    bool pdf = source.suffix().compare(QStringLiteral("pdf"), Qt::CaseInsensitive) == 0;
    if (!document && !pdf) {
        QFile header(input);
        pdf = header.open(QIODevice::ReadOnly) && header.read(5) == QByteArrayLiteral("%PDF-");
    }
    if ((!document && !pdf) || !source.isFile() || !source.isReadable()
        || source.size() > 64LL * 1024 * 1024)
        return {Outcome::Failure, QStringLiteral("Documento inválido ou maior que 64 MiB.")};
    if (QFileInfo::exists(privateOutputPath)
        || source.absoluteFilePath() == QFileInfo(privateOutputPath).absoluteFilePath())
        return {Outcome::Failure, QStringLiteral("O destino temporário já existe.")};
    const auto supported = pdf && !document ? pdfOutputFormats() : outputFormats();
    if (!supported.contains(normalized))
        return {Outcome::Failure, QStringLiteral("Este formato de documento não está disponível.")};
    QByteArray output;
    const Runtime config = runtime();
    const QStringList arguments{QStringLiteral("--input"), source.absoluteFilePath(),
        QStringLiteral("--output"), QFileInfo(privateOutputPath).absoluteFilePath(),
        QStringLiteral("--format"), normalized, QStringLiteral("--soffice"), config.soffice};
    const int timeout = pdf && !document ? 1800000 : 180000;
    Outcome outcome = session ? session->run(arguments, cancel, timeout, progress)
                              : run(arguments, cancel, output, timeout, progress);
    if (outcome.status == Outcome::Success
        && (!QFileInfo(privateOutputPath).isFile() || QFileInfo(privateOutputPath).size() == 0))
        outcome = {Outcome::Failure, QStringLiteral("A conversão não produziu um documento válido.")};
    if (outcome.status != Outcome::Success) QFile::remove(privateOutputPath);
    return outcome;
}

Outcome convertImage(const QString &preparedPng, const QString &format,
                     const QString &privateOutputPath, std::atomic_bool &cancel, const Progress &progress, Session *session)
{
    if (cancel.load()) return {Outcome::Cancelled, {}};
    const QFileInfo source(preparedPng), destination(privateOutputPath);
    if (!imageOcrFormats().contains(format) || !source.isFile() || !source.isReadable()
        || source.size() > 64LL * 1024 * 1024 || destination.exists() || destination.isSymLink())
        return {Outcome::Failure, QStringLiteral("Não foi possível preparar o reconhecimento de texto da imagem.")};
    const auto config = runtime();
    QByteArray output;
    const QStringList arguments{QStringLiteral("--image-ocr"), QStringLiteral("--input"), source.absoluteFilePath(),
        QStringLiteral("--output"), destination.absoluteFilePath(), QStringLiteral("--format"), format,
        QStringLiteral("--soffice"), config.soffice};
    auto outcome = session ? session->run(arguments, cancel, 120000, progress)
                           : run(arguments, cancel, output, 120000, progress);
    if (outcome.status == Outcome::Success
        && (!QFileInfo(privateOutputPath).isFile() || QFileInfo(privateOutputPath).size() == 0))
        outcome = {Outcome::Failure, QStringLiteral("O OCR não produziu texto legível nesta imagem.")};
    if (outcome.status != Outcome::Success) QFile::remove(privateOutputPath);
    return outcome;
}

} // namespace Mandarina::Documents
