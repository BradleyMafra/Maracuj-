#include "audioconversion.h"
#include "runtimepaths.h"
#include "ownedprocess.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>

namespace Mandarina::Audio {
namespace {

constexpr qsizetype MaxCapturedBytes = 256 * 1024;
constexpr qint64 ConversionTimeoutMs = 6LL * 60 * 60 * 1000;

enum class ProcessStatus { Success, Failure, Cancelled, TimedOut };

struct Profile {
    QString format;
    QString encoder;
    QString muxer;
    QStringList options;
    bool stereoMaximum = false;
};

ProcessStatus runProcess(const QString &program, const QStringList &arguments,
                         std::atomic_bool &cancel, qint64 timeoutMs,
                         QByteArray *captured = nullptr)
{
    if (cancel.load()) return ProcessStatus::Cancelled;
    if (!QFileInfo(program).isExecutable()) return ProcessStatus::Failure;
    OwnedProcess process;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    // FFREPORT can otherwise write paths and metadata to an unsolicited report.
    environment.remove(QStringLiteral("FFREPORT"));
    environment.insert(QStringLiteral("AV_LOG_FORCE_NOCOLOR"), QStringLiteral("1"));
    process.setProcessEnvironment(environment);
    process.setProgram(program);
    process.setArguments(arguments);
    process.setStandardErrorFile(QProcess::nullDevice());
    process.setProcessChannelMode(QProcess::SeparateChannels);
    if (!captured) process.setStandardOutputFile(QProcess::nullDevice());
    QElapsedTimer elapsed;
    elapsed.start();
    if (!process.startOwned()) {
        process.stopOwned();
        return cancel.load() ? ProcessStatus::Cancelled : ProcessStatus::Failure;
    }
    const auto drain = [&]() {
        if (!captured) return true;
        const QByteArray bytes = process.readAllStandardOutput();
        if (bytes.size() > MaxCapturedBytes - captured->size()) return false;
        captured->append(bytes);
        return true;
    };
    while (process.state() != QProcess::NotRunning) {
        process.waitForFinished(75);
        if (!drain()) {
            process.stopOwned();
            return ProcessStatus::Failure;
        }
        if (cancel.load()) {
            process.stopOwned();
            return ProcessStatus::Cancelled;
        }
        if (elapsed.elapsed() > timeoutMs) {
            process.stopOwned();
            return ProcessStatus::TimedOut;
        }
    }
    if (!drain()) return ProcessStatus::Failure;
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0
        ? ProcessStatus::Success : ProcessStatus::Failure;
}

QString demuxerFor(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QStringLiteral("mp3")) return QStringLiteral("mp3");
    if (suffix == QStringLiteral("wav")) return QStringLiteral("wav");
    if (suffix == QStringLiteral("flac")) return QStringLiteral("flac");
    if (suffix == QStringLiteral("ogg") || suffix == QStringLiteral("oga")
        || suffix == QStringLiteral("opus")) return QStringLiteral("ogg");
    if (suffix == QStringLiteral("m4a") || suffix == QStringLiteral("m4b"))
        return QStringLiteral("mov");
    if (suffix == QStringLiteral("aiff") || suffix == QStringLiteral("aif"))
        return QStringLiteral("aiff");
    if (suffix == QStringLiteral("wma")) return QStringLiteral("asf");
    if (suffix == QStringLiteral("aac")) return QStringLiteral("aac");
    return {};
}

QList<Profile> detectProfiles()
{
    if (!QFileInfo(RuntimePaths::tool(QStringLiteral("ffprobe"))).isExecutable()) return {};
    std::atomic_bool noCancel = false;
    QByteArray encoderOutput;
    QByteArray muxerOutput;
    if (runProcess(RuntimePaths::tool(QStringLiteral("ffmpeg")),
        {QStringLiteral("-nostdin"), QStringLiteral("-hide_banner"), QStringLiteral("-encoders")},
        noCancel, 10000, &encoderOutput) != ProcessStatus::Success) return {};
    if (runProcess(RuntimePaths::tool(QStringLiteral("ffmpeg")),
        {QStringLiteral("-nostdin"), QStringLiteral("-hide_banner"), QStringLiteral("-muxers")},
        noCancel, 10000, &muxerOutput) != ProcessStatus::Success) return {};

    QSet<QString> encoders;
    QSet<QString> muxers;
    const QRegularExpression encoderExpression(QStringLiteral("^\\s*A[.A-Z]{5}\\s+(\\S+)"),
                                               QRegularExpression::MultilineOption);
    const QString encoderText = QString::fromLatin1(encoderOutput);
    auto encoderMatches = encoderExpression.globalMatch(encoderText);
    while (encoderMatches.hasNext()) encoders.insert(encoderMatches.next().captured(1));
    const QRegularExpression muxerExpression(QStringLiteral("^\\s*E\\s+(\\S+)"),
                                             QRegularExpression::MultilineOption);
    const QString muxerText = QString::fromLatin1(muxerOutput);
    auto muxerMatches = muxerExpression.globalMatch(muxerText);
    while (muxerMatches.hasNext()) {
        const auto names = muxerMatches.next().captured(1).split(QLatin1Char(','));
        for (const QString &name : names) muxers.insert(name);
    }

    const QList<Profile> candidates = {
        {QStringLiteral("mp3"), QStringLiteral("libmp3lame"), QStringLiteral("mp3"),
         {QStringLiteral("-b:a"), QStringLiteral("192k"), QStringLiteral("-ar"), QStringLiteral("44100")}, true},
        {QStringLiteral("m4a"), QStringLiteral("aac"), QStringLiteral("ipod"),
         {QStringLiteral("-b:a"), QStringLiteral("192k"), QStringLiteral("-ar"), QStringLiteral("48000")}, true},
        {QStringLiteral("wav"), QStringLiteral("pcm_s24le"), QStringLiteral("wav"), {}, false},
        {QStringLiteral("flac"), QStringLiteral("flac"), QStringLiteral("flac"),
         {QStringLiteral("-compression_level"), QStringLiteral("5")}, false},
        {QStringLiteral("ogg"), QStringLiteral("libvorbis"), QStringLiteral("ogg"),
         {QStringLiteral("-q:a"), QStringLiteral("5"), QStringLiteral("-ar"), QStringLiteral("48000")}, true},
        {QStringLiteral("opus"), QStringLiteral("libopus"), QStringLiteral("opus"),
         {QStringLiteral("-b:a"), QStringLiteral("128k"), QStringLiteral("-ar"), QStringLiteral("48000")}, true},
        {QStringLiteral("aiff"), QStringLiteral("pcm_s24be"), QStringLiteral("aiff"), {}, false},
        {QStringLiteral("wma"), QStringLiteral("wmav2"), QStringLiteral("asf"),
         {QStringLiteral("-b:a"), QStringLiteral("192k"), QStringLiteral("-ar"), QStringLiteral("44100")}, true}
    };
    QList<Profile> result;
    for (const Profile &profile : candidates)
        if (encoders.contains(profile.encoder) && muxers.contains(profile.muxer)) result.append(profile);
    return result;
}

const QList<Profile> &profiles()
{
    static const QList<Profile> result = detectProfiles();
    return result;
}

Outcome failure(const QString &error)
{
    return {Status::Failure, error};
}

Outcome processFailure(ProcessStatus status)
{
    if (status == ProcessStatus::Cancelled) return {Status::Cancelled, {}};
    if (status == ProcessStatus::TimedOut)
        return failure(QStringLiteral("A conversão de áudio excedeu o tempo máximo e foi interrompida."));
    return failure(QStringLiteral("Não foi possível converter este áudio. O arquivo pode estar inválido ou usar um codec não compatível."));
}

} // namespace

bool isSource(const QString &path)
{
    return !demuxerFor(path).isEmpty();
}

QStringList outputFormats()
{
    QStringList result;
    for (const Profile &profile : profiles()) result.append(profile.format);
    return result;
}

Outcome convert(const QString &input, const QString &format,
                const QString &privateOutputPath, std::atomic_bool &cancel)
{
    if (cancel.load()) return {Status::Cancelled, {}};
    const QFileInfo source(input);
    const QFileInfo destination(privateOutputPath);
    const QString demuxer = demuxerFor(input);
    if (demuxer.isEmpty() || !source.isFile() || !source.isReadable() || source.size() <= 0)
        return failure(QStringLiteral("Escolha um arquivo de áudio local, legível e não vazio."));
    if (destination.exists() || destination.isSymLink()
        || !destination.absoluteDir().exists())
        return failure(QStringLiteral("Não foi possível preparar o arquivo temporário de áudio."));
    const QString normalizedFormat = format.trimmed().toLower();
    const auto &available = profiles();
    const auto selected = std::find_if(available.cbegin(), available.cend(),
        [&](const Profile &profile) { return profile.format == normalizedFormat; });
    if (selected == available.cend())
        return failure(QStringLiteral("Este formato de áudio não está disponível no FFmpeg instalado."));

    // Force a single-file demuxer and local file protocol. A renamed playlist
    // must never turn a local conversion into requests for external resources.
    const QStringList inputOptions = {
        QStringLiteral("-protocol_whitelist"), QStringLiteral("file"),
        QStringLiteral("-f"), demuxer,
        QStringLiteral("-probesize"), QStringLiteral("5000000"),
        QStringLiteral("-analyzeduration"), QStringLiteral("10000000")
    };
    QByteArray probeOutput;
    QStringList probeArguments = {QStringLiteral("-v"), QStringLiteral("error")};
    probeArguments += inputOptions;
    probeArguments += {QStringLiteral("-select_streams"), QStringLiteral("a:0"),
        QStringLiteral("-show_entries"), QStringLiteral("stream=codec_type,codec_name,sample_rate,channels:format=duration"),
        QStringLiteral("-of"), QStringLiteral("json"), QStringLiteral("-i"), source.absoluteFilePath()};
    const ProcessStatus probeStatus = runProcess(RuntimePaths::tool(QStringLiteral("ffprobe")), probeArguments,
                                                 cancel, 30000, &probeOutput);
    if (probeStatus != ProcessStatus::Success) return processFailure(probeStatus);
    const QJsonArray streams = QJsonDocument::fromJson(probeOutput).object()
                                  .value(QStringLiteral("streams")).toArray();
    if (streams.size() != 1)
        return failure(QStringLiteral("O arquivo não contém uma faixa de áudio compatível."));
    const QJsonObject stream = streams.first().toObject();
    bool rateOk = false;
    const int sampleRate = stream.value(QStringLiteral("sample_rate")).toString().toInt(&rateOk);
    const int channels = stream.value(QStringLiteral("channels")).toInt();
    if (stream.value(QStringLiteral("codec_type")).toString() != QStringLiteral("audio")
        || stream.value(QStringLiteral("codec_name")).toString().isEmpty()
        || !rateOk || sampleRate < 1 || sampleRate > 384000 || channels < 1 || channels > 8)
        return failure(QStringLiteral("A faixa de áudio tem parâmetros não compatíveis com esta versão."));

    QStringList arguments = {QStringLiteral("-nostdin"), QStringLiteral("-hide_banner"),
        QStringLiteral("-loglevel"), QStringLiteral("error"), QStringLiteral("-nostats"),
        QStringLiteral("-xerror"), QStringLiteral("-n"), QStringLiteral("-threads"), QStringLiteral("1"),
        QStringLiteral("-filter_threads"), QStringLiteral("1")};
    arguments += inputOptions;
    arguments += {QStringLiteral("-i"), source.absoluteFilePath(),
        QStringLiteral("-map"), QStringLiteral("0:a:0"),
        QStringLiteral("-vn"), QStringLiteral("-sn"), QStringLiteral("-dn"),
        QStringLiteral("-map_metadata"), QStringLiteral("-1"),
        QStringLiteral("-map_metadata:s:a"), QStringLiteral("-1"),
        QStringLiteral("-map_chapters"), QStringLiteral("-1"),
        QStringLiteral("-c:a"), selected->encoder,
        QStringLiteral("-threads:a"), QStringLiteral("1")};
    if (selected->stereoMaximum)
        arguments += {QStringLiteral("-ac"), QString::number(std::min(channels, 2))};
    arguments += selected->options;
    arguments += {QStringLiteral("-f"), selected->muxer, destination.absoluteFilePath()};
    const ProcessStatus status = runProcess(RuntimePaths::tool(QStringLiteral("ffmpeg")), arguments,
                                             cancel, ConversionTimeoutMs);
    if (status != ProcessStatus::Success) return processFailure(status);
    if (cancel.load()) return {Status::Cancelled, {}};
    const QFileInfo output(destination.absoluteFilePath());
    if (!output.isFile() || output.isSymLink() || output.size() <= 0)
        return failure(QStringLiteral("O conversor não produziu um arquivo de áudio válido."));
    return {Status::Success, {}};
}

} // namespace Mandarina::Audio
