#include "filetoolsengine.h"
#include "runtimepaths.h"
#include "ownedprocess.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QTemporaryDir>

namespace Mandarina::FileTools {
namespace {
constexpr qint64 MaxFileBytes = 512LL * 1024 * 1024;
constexpr qint64 MaxTotalBytes = 2LL * 1024 * 1024 * 1024;
constexpr qsizetype MaxResponse = 4 * 1024 * 1024;

QJsonObject invoke(const QJsonObject &request, QTemporaryDir &temporary,
                   std::atomic_bool &cancel, QString &error)
{
    if (cancel.load()) return {};
    const QString helper = RuntimePaths::helper(QStringLiteral("file_tools.py"));
    if (!QFileInfo(RuntimePaths::python()).isExecutable() || !QFileInfo(helper).isFile()) {
        error = QStringLiteral("O componente local de ferramentas de arquivos não está disponível.");
        return {};
    }
    QFile input(temporary.filePath(QStringLiteral("request.json")));
    if (!input.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || input.write(QJsonDocument(request).toJson(QJsonDocument::Compact)) < 0) {
        error = QStringLiteral("Não foi possível preparar a ferramenta local.");
        return {};
    }
    input.close();
    OwnedProcess process;
    auto environment = QProcessEnvironment::systemEnvironment();
#ifndef Q_OS_WIN
    environment.remove(QStringLiteral("LD_LIBRARY_PATH"));
#endif
    environment.insert(QStringLiteral("PYTHONDONTWRITEBYTECODE"), QStringLiteral("1"));
    environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C.UTF-8"));
    environment.insert(QStringLiteral("OMP_NUM_THREADS"), QStringLiteral("1"));
    process.setProcessEnvironment(environment);
    process.setWorkingDirectory(temporary.path());
    process.setProgram(RuntimePaths::python());
    process.setArguments({QStringLiteral("-I"), helper, QStringLiteral("--request"), input.fileName(),
                          QStringLiteral("--runtime-root"), RuntimePaths::root()});
    process.setProcessChannelMode(QProcess::SeparateChannels);
    if (!process.startOwned()) {
        error = QStringLiteral("Não foi possível iniciar a ferramenta local.");
        return {};
    }
    QElapsedTimer elapsed;
    elapsed.start();
    QByteArray output;
    bool invalid = false;
    while (process.state() != QProcess::NotRunning) {
        process.waitForFinished(75);
        output.append(process.readAllStandardOutput());
        process.readAllStandardError(); // Private paths/data never become diagnostics.
        if (cancel.load() || output.size() > MaxResponse || elapsed.elapsed() > 15 * 60 * 1000) {
            invalid = !cancel.load();
            process.stopOwned();
            break;
        }
    }
    output.append(process.readAllStandardOutput());
    process.readAllStandardError();
    process.finishOwnedTree();
    if (cancel.load()) return {};
    if (invalid || output.size() > MaxResponse) {
        error = QStringLiteral("A ferramenta excedeu o limite de tempo ou de recursos.");
        return {};
    }
    const auto result = QJsonDocument::fromJson(output.trimmed()).object();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0
        || !result.value(QStringLiteral("ok")).toBool()) {
        error = result.value(QStringLiteral("error")).toString();
        if (error.isEmpty() || error.size() > 512)
            error = QStringLiteral("Não foi possível processar este arquivo.");
        return {};
    }
    return result;
}

QString outputExtension(const Request &request)
{
    if (request.action == QStringLiteral("metadata")) return QFileInfo(request.paths.first()).suffix();
    if (request.paths.size() != 1 || request.options.value(QStringLiteral("mode")).toString() == QStringLiteral("zip"))
        return QStringLiteral("zip");
    const QString extension = QFileInfo(request.paths.first()).suffix().toLower();
    return QStringList{QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("png"),
                       QStringLiteral("webp"), QStringLiteral("pdf")}.contains(extension) ? extension : QStringLiteral("zip");
}

QString outputBase(const Request &request)
{
    QString base = request.paths.size() > 1 ? QStringLiteral("arquivos") : QFileInfo(request.paths.first()).completeBaseName();
    base.replace(QRegularExpression(QStringLiteral("[<>:\"|?*\\x00-\\x1f]")), QStringLiteral("_"));
    base = base.trimmed().left(120);
    while (base.startsWith(QLatin1Char('.'))) base.remove(0, 1);
    while (base.endsWith(QLatin1Char('.')) || base.endsWith(QLatin1Char(' '))) base.chop(1);
    if (base.isEmpty()) base = QStringLiteral("arquivo");
    return base + (request.action == QStringLiteral("metadata") ? QStringLiteral("-metadados") : QStringLiteral("-comprimido"));
}

QString publish(const QString &source, const QDir &directory, const QString &base, const QString &extension)
{
    for (int sequence = 1; sequence <= 10000; ++sequence) {
        const QString name = base + (sequence > 1 ? QStringLiteral(" (%1)").arg(sequence) : QString{})
            + (extension.isEmpty() ? QString{} : QLatin1Char('.') + extension);
        const QString target = directory.absoluteFilePath(name);
        if (QFileInfo::exists(target) || QFileInfo(target).isSymLink()) continue;
        // Refuses replacement on every platform; fallback cross-device copying
        // is handled by QFile, retaining the private source until publication.
        if (QFile::rename(source, target)) return target;
        if (!QFileInfo::exists(target)) return {};
    }
    return {};
}
} // namespace

Outcome inspect(const QString &path, std::atomic_bool &cancel)
{
    Outcome result;
    const QFileInfo source(path);
    if (!source.isFile() || source.isSymLink() || !source.isReadable() || source.size() > 128LL * 1024 * 1024) {
        result.errors.append(QStringLiteral("Selecione um arquivo local de até 128 MiB para inspecionar os metadados."));
        return result;
    }
    QTemporaryDir temporary;
    if (!temporary.isValid()) {
        result.errors.append(QStringLiteral("Não foi possível criar uma área de trabalho privada."));
        return result;
    }
    QString error;
    const auto response = invoke({{QStringLiteral("action"), QStringLiteral("inspect")},
        {QStringLiteral("input"), source.absoluteFilePath()}}, temporary, cancel, error);
    result.cancelled = cancel.load();
    if (!error.isEmpty()) result.errors.append(error);
    result.metadata = response.value(QStringLiteral("metadata")).toArray().toVariantList();
    if (response.contains(QStringLiteral("details")))
        result.details.append(response.value(QStringLiteral("details")).toObject().toVariantMap());
    return result;
}

Outcome run(const Request &request, std::atomic_bool &cancel, const Progress &progress)
{
    Outcome result;
    if ((request.action != QStringLiteral("compress") && request.action != QStringLiteral("metadata"))
        || request.paths.isEmpty() || request.paths.size() > 128
        || (request.action == QStringLiteral("metadata") && request.paths.size() != 1)) {
        result.errors.append(QStringLiteral("Escolha arquivos e uma ferramenta válida; os metadados são editados um arquivo por vez."));
        return result;
    }
    qint64 total = 0;
    for (const auto &path : request.paths) {
        const QFileInfo file(path);
        if (!file.isFile() || file.isSymLink() || !file.isReadable() || file.size() > MaxFileBytes) {
            result.errors.append(QStringLiteral("Um arquivo não está disponível ou excede o limite de 512 MiB."));
            return result;
        }
        total += file.size();
    }
    if (total > MaxTotalBytes) {
        result.errors.append(QStringLiteral("O conjunto de arquivos excede o limite de 2 GiB."));
        return result;
    }
    const QString extension = outputExtension(request);
    const bool archive = request.action == QStringLiteral("compress") && extension == QStringLiteral("zip");
    const int jobs = archive ? 1 : request.paths.size();
    for (int index = 0; index < jobs && !cancel.load(); ++index) {
        Request item = request;
        if (!archive) item.paths = {request.paths.at(index)};
        const QDir directory(request.outputDirectory.trimmed().isEmpty()
            ? QFileInfo(item.paths.first()).absolutePath() : request.outputDirectory);
        if (!directory.exists() || !QFileInfo(directory.absolutePath()).isWritable()) {
            result.errors.append(QStringLiteral("A pasta de saída não está disponível para gravação."));
            break;
        }
        QTemporaryDir temporary(directory.absoluteFilePath(QStringLiteral(".maracuja-tools-XXXXXX")));
        if (!temporary.isValid()) {
            result.errors.append(QStringLiteral("Não foi possível criar uma área de trabalho privada."));
            break;
        }
        if (progress) progress(index, jobs, QFileInfo(item.paths.first()).fileName());
        QJsonArray paths;
        for (const auto &path : item.paths) paths.append(QFileInfo(path).absoluteFilePath());
        const QString staged = temporary.filePath(QStringLiteral("result.") + extension);
        QString error;
        const auto response = invoke({{QStringLiteral("action"), item.action},
            {QStringLiteral("inputs"), paths}, {QStringLiteral("output"), staged},
            {QStringLiteral("options"), QJsonObject::fromVariantMap(item.options)}}, temporary, cancel, error);
        if (cancel.load()) break;
        if (!error.isEmpty() || !QFileInfo(staged).isFile() || QFileInfo(staged).size() <= 0) {
            result.errors.append(error.isEmpty() ? QStringLiteral("A ferramenta não produziu um arquivo válido.") : error);
            continue;
        }
        const QString published = publish(staged, directory, outputBase(item), extension);
        if (published.isEmpty()) {
            result.errors.append(QStringLiteral("Não foi possível salvar o resultado sem substituir um arquivo existente."));
            continue;
        }
        result.outputs.append(published);
        auto details = response.value(QStringLiteral("details")).toObject().toVariantMap();
        details.insert(QStringLiteral("output"), published);
        details.insert(QStringLiteral("input"), item.paths.first());
        result.details.append(details);
        if (progress) progress(index + 1, jobs, QFileInfo(published).fileName());
    }
    result.cancelled = cancel.load();
    return result;
}
} // namespace Mandarina::FileTools
