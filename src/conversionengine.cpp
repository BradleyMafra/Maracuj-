#include "conversionengine.h"
#include "audioconversion.h"
#include "documentconversion.h"
#include "runtimepaths.h"
#include "ownedprocess.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringConverter>
#include <QTemporaryDir>
#include <QTextStream>
#include <algorithm>
#include <cmath>

namespace Mandarina {
namespace {

constexpr qint64 MaxPixels = 64LL * 1000 * 1000;
constexpr qint64 MaxImageBytes = 256LL * 1024 * 1024;

QString normalizedFormat(QString format)
{
    format = format.trimmed().toLower();
    if (format == QStringLiteral("jpeg")) return QStringLiteral("jpg");
    if (format == QStringLiteral("tif")) return QStringLiteral("tiff");
    if (format == QStringLiteral("heif")) return QStringLiteral("heic");
    return format;
}

QByteArray writerFormat(const QString &format)
{
    const auto formats = QImageWriter::supportedImageFormats();
    const QByteArray requested = format.toLatin1();
    if (formats.contains(requested)) return requested;
    if (format == QStringLiteral("jpg") && formats.contains("jpeg")) return "jpeg";
    if (format == QStringLiteral("tiff") && formats.contains("tif")) return "tif";
    if (format == QStringLiteral("heic") && formats.contains("heif")) return "heif";
    return {};
}

QString outputBase(const QString &input)
{
    QString base = QFileInfo(input).completeBaseName();
    base.replace(QRegularExpression(QStringLiteral("[\\x00-\\x1f\\x7f/\\\\]")), QStringLiteral("_"));
    #ifdef Q_OS_WIN
    base.replace(QRegularExpression(QStringLiteral("[<>:\"|?*]")), QStringLiteral("_"));
#endif
    base = base.trimmed();
    while (base.startsWith(QLatin1Char('.'))) base.remove(0, 1);
    while (base.endsWith(QLatin1Char('.')) || base.endsWith(QLatin1Char(' '))) base.chop(1);
    if (base.isEmpty()) base = QStringLiteral("arquivo");
    return base.left(140) + QStringLiteral("-convertido");
}

QString candidatePath(const QDir &directory, const QString &base,
                      const QString &extension, int sequence)
{
    QString name = base;
    if (sequence > 1) name += QStringLiteral(" (%1)").arg(sequence);
    if (!extension.isEmpty()) name += QLatin1Char('.') + extension;
    return directory.absoluteFilePath(name);
}

QString publishFile(const QString &temporary, const QDir &directory,
                    const QString &base, const QString &extension)
{
    for (int sequence = 1; sequence <= 10000; ++sequence) {
        const QString destination = candidatePath(directory, base, extension, sequence);
        if (QFileInfo::exists(destination)) continue;
        // QFile::rename refuses an existing destination, unlike a plain POSIX rename.
        if (QFile::rename(temporary, destination)) return destination;
        if (!QFileInfo::exists(destination)) return {};
    }
    return {};
}

enum class ProcessResult { Success, Failure, Cancelled };

ProcessResult runProcess(const QString &program, const QStringList &arguments,
                         std::atomic_bool &cancel, QByteArray *standardOutput = nullptr)
{
    if (cancel.load()) return ProcessResult::Cancelled;
    if (!QFileInfo(program).isExecutable()) return ProcessResult::Failure;
    OwnedProcess process;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    process.setProcessEnvironment(environment);
    process.setProgram(program);
    process.setArguments(arguments);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    if (!process.startOwned()) return ProcessResult::Failure;
    while (process.state() != QProcess::NotRunning) {
        process.waitForFinished(75);
        // Drain output without logging document contents, metadata or private paths.
        const QByteArray output = process.readAllStandardOutput();
        if (standardOutput) {
            standardOutput->append(output);
            if (standardOutput->size() > 4 * 1024 * 1024) {
                process.stopOwned();
                return ProcessResult::Failure;
            }
        }
        process.readAllStandardError();
        if (cancel.load()) {
            process.stopOwned();
            return ProcessResult::Cancelled;
        }
    }
    if (standardOutput) standardOutput->append(process.readAllStandardOutput());
    else process.readAllStandardOutput();
    process.readAllStandardError();
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0
        ? ProcessResult::Success : ProcessResult::Failure;
}

ProcessResult pdfRenderDpi(const QString &input, int requestedDpi, double &dpi,
                           std::atomic_bool &cancel)
{
    QByteArray output;
    const ProcessResult status = runProcess(RuntimePaths::tool(QStringLiteral("pdfinfo")),
        {QStringLiteral("-f"), QStringLiteral("1"), QStringLiteral("-l"),
         QStringLiteral("1000000"), input}, cancel, &output);
    if (status != ProcessResult::Success) return status;
    const QString metadata = QString::fromLocal8Bit(output);
    const QRegularExpression pageCountExpression(QStringLiteral("^Pages:\\s*(\\d+)"),
                                                 QRegularExpression::MultilineOption);
    const auto pageCountMatch = pageCountExpression.match(metadata);
    bool pageCountOk = false;
    const int pageCount = pageCountMatch.captured(1).toInt(&pageCountOk);
    if (!pageCountOk || pageCount < 1 || pageCount > 1000000) return ProcessResult::Failure;
    const QRegularExpression pageSizeExpression(
        QStringLiteral("^Page\\s+(?:\\d+\\s+)?size:\\s+([0-9.]+)\\s+x\\s+([0-9.]+)\\s+pts"),
        QRegularExpression::MultilineOption);
    auto matches = pageSizeExpression.globalMatch(metadata);
    int dimensionsFound = 0;
    double largestSide = 0;
    while (matches.hasNext()) {
        const auto match = matches.next();
        const double width = match.captured(1).toDouble();
        const double height = match.captured(2).toDouble();
        if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0)
            return ProcessResult::Failure;
        largestSide = std::max({largestSide, width, height});
        ++dimensionsFound;
    }
    if (dimensionsFound != pageCount || largestSide <= 0) return ProcessResult::Failure;
    // PDF points are 1/72 inch. A one-pixel margin avoids rounding above 4096.
    dpi = std::min(double(std::clamp(requestedDpi, 72, 300)), 4095.0 * 72.0 / largestSide);
    return std::isfinite(dpi) && dpi > 0 ? ProcessResult::Success : ProcessResult::Failure;
}

QImage readImage(const QString &input, QString &error)
{
    QImageReader reader(input);
    reader.setAutoTransform(true);
    const QSize dimensions = reader.size();
    if (!dimensions.isValid() || dimensions.isEmpty()) {
        error = QStringLiteral("Não foi possível ler as dimensões desta imagem.");
        return {};
    }
    if (qint64(dimensions.width()) * qint64(dimensions.height()) > MaxPixels) {
        error = QStringLiteral("A imagem excede o limite de 64 megapixels desta versão.");
        return {};
    }
    if (reader.imageCount() > 1) {
        error = QStringLiteral("Imagens animadas ou com várias páginas ainda não são compatíveis.");
        return {};
    }
    QImage image = reader.read();
    if (image.isNull()) {
        error = QStringLiteral("Não foi possível abrir a imagem; o arquivo pode estar inválido ou exigir memória demais.");
        return {};
    }
    if (qint64(image.width()) * qint64(image.height()) > MaxPixels
        || image.sizeInBytes() > MaxImageBytes) {
        error = QStringLiteral("A imagem excede o limite de memória desta versão.");
        return {};
    }
    return image;
}

QImage flattenOnWhite(const QImage &image)
{
    if (!image.hasAlphaChannel()) return image;
    QImage flattened(image.size(), QImage::Format_RGB32);
    if (flattened.isNull()) return {};
    flattened.fill(Qt::white);
    QPainter painter(&flattened);
    painter.drawImage(0, 0, image);
    painter.end();
    return flattened;
}

void report(const Progress &progress, int done, int total, const QString &message)
{
    if (progress) progress(done, total, message);
}

void appendError(Result &result, int index, const QString &message)
{
    result.errors.append(QStringLiteral("Arquivo %1: %2").arg(index + 1).arg(message));
}

} // namespace

QString sourceKind(const QString &path)
{
    const QFileInfo info(path);
    if (!info.isFile() || !info.isReadable()) return QStringLiteral("unsupported");
    // A text document can legitimately discuss the literal PDF header.
    if (Documents::isSource(path)) return QStringLiteral("document");
    QFile input(info.absoluteFilePath());
    if (!input.open(QIODevice::ReadOnly)) return QStringLiteral("unsupported");
    const auto header = input.read(1024);
    if (header.startsWith("%PDF-") || (info.suffix().compare(QStringLiteral("pdf"), Qt::CaseInsensitive) == 0
        && header.contains("%PDF-"))) return QStringLiteral("pdf");
    input.close();
    if (Audio::isSource(path)) return QStringLiteral("audio");
    QImageReader reader(info.absoluteFilePath());
    return reader.canRead() ? QStringLiteral("image") : QStringLiteral("unsupported");
}

QStringList imageOutputFormats()
{
    QStringList result;
    for (const QString &format : {QStringLiteral("jpg"), QStringLiteral("png"),
                                 QStringLiteral("webp"), QStringLiteral("bmp"),
                                 QStringLiteral("tiff"), QStringLiteral("avif"),
                                 QStringLiteral("heic")}) {
        if (!writerFormat(format).isEmpty()) result.append(format);
    }
    return result;
}

QStringList outputFormats(const QString &kind)
{
    if (kind == QStringLiteral("image")) {
        auto formats = imageOutputFormats();
        formats.append(QStringLiteral("pdf"));
        formats.append(Documents::imageOcrFormats());
        return formats;
    }
    if (kind == QStringLiteral("audio")) return Audio::outputFormats();
    if (kind == QStringLiteral("document")) {
        auto formats = Documents::outputFormats();
        if (formats.contains(QStringLiteral("pdf"))) formats << QStringLiteral("png") << QStringLiteral("jpg");
        return formats;
    }
    if (kind == QStringLiteral("pdf")) {
        QStringList formats{QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("txt"), QStringLiteral("pdf")};
        formats.append(Documents::pdfOutputFormats());
        formats.removeDuplicates();
        return formats;
    }
    return {};
}

Result convert(const Request &request, std::atomic_bool &cancel, const Progress &progress)
{
    Result result;
    if (cancel.load()) { result.cancelled = true; return result; }
    if (request.inputFiles.isEmpty()) {
        result.errors.append(QStringLiteral("Selecione pelo menos um arquivo para converter."));
        return result;
    }
    QImageReader::setAllocationLimit(256);
    const QString format = normalizedFormat(request.format);
    QStringList inputs;
    QStringList kinds;
    for (const QString &input : request.inputFiles) {
        inputs.append(QFileInfo(input).absoluteFilePath());
        kinds.append(sourceKind(inputs.last()));
    }
    const QString destination = request.outputDir.trimmed().isEmpty()
        ? QFileInfo(inputs.first()).absolutePath() : QFileInfo(request.outputDir).absoluteFilePath();
    if (!QDir().mkpath(destination) || !QFileInfo(destination).isDir()) {
        result.errors.append(QStringLiteral("Não foi possível abrir ou criar a pasta de destino."));
        return result;
    }
    const QDir directory(destination);
    const bool allImages = std::all_of(kinds.cbegin(), kinds.cend(), [](const QString &kind) { return kind == QStringLiteral("image"); });
    const bool allPdfs = std::all_of(kinds.cbegin(), kinds.cend(), [](const QString &kind) { return kind == QStringLiteral("pdf"); });

    if (format == QStringLiteral("pdf") && allImages) {
        // Publish only after QPdfWriter has closed its complete output.
        QTemporaryDir temporary(directory.absoluteFilePath(QStringLiteral(".mandarina-XXXXXX")));
        if (!temporary.isValid()) {
            result.errors.append(QStringLiteral("Não foi possível criar um arquivo temporário na pasta de destino."));
            return result;
        }
        QString temporaryPdf = temporary.filePath(QStringLiteral("saida.pdf"));
        {
            QPdfWriter writer(temporaryPdf);
            writer.setTitle(QStringLiteral("Imagens convertidas"));
            writer.setCreator(QStringLiteral("Maracujá"));
            writer.setResolution(std::clamp(request.dpi, 72, 300));
            writer.setPageSize(QPageSize(QPageSize::A4));
            writer.setPageMargins(QMarginsF(10, 10, 10, 10), QPageLayout::Millimeter);
            QPainter painter;
            for (int index = 0; index < inputs.size(); ++index) {
                if (cancel.load()) { result.cancelled = true; break; }
                report(progress, index, inputs.size(), QStringLiteral("Montando PDF — imagem %1 de %2").arg(index + 1).arg(inputs.size()));
                QString error;
                const QImage image = readImage(inputs.at(index), error);
                if (image.isNull()) { appendError(result, index, error); break; }
                writer.setPageOrientation(image.width() > image.height() ? QPageLayout::Landscape : QPageLayout::Portrait);
                if (index == 0 ? !painter.begin(&writer) : !writer.newPage()) {
                    result.errors.append(QStringLiteral("Não foi possível criar uma página do PDF."));
                    break;
                }
                const QRect page(0, 0, writer.width(), writer.height());
                const QSize fitted = image.size().scaled(page.size(), Qt::KeepAspectRatio);
                const QRect target(QPoint((page.width() - fitted.width()) / 2,
                                          (page.height() - fitted.height()) / 2), fitted);
                painter.fillRect(page, Qt::white);
                painter.drawImage(target, image);
                report(progress, index + 1, inputs.size(), QStringLiteral("Imagem adicionada ao PDF"));
            }
            if (painter.isActive() && !painter.end() && result.errors.isEmpty())
                result.errors.append(QStringLiteral("Não foi possível finalizar o PDF."));
        }
        if (cancel.load()) result.cancelled = true;
        if (result.cancelled || !result.errors.isEmpty()) return result;
        const QString published = publishFile(temporaryPdf, directory, outputBase(inputs.first()), QStringLiteral("pdf"));
        if (published.isEmpty()) result.errors.append(QStringLiteral("Não foi possível salvar o PDF sem substituir um arquivo existente."));
        else result.outputs.append(published);
        return result;
    }

    if (format == QStringLiteral("pdf") && allPdfs) {
        if (inputs.size() > 1 && !QFileInfo(RuntimePaths::tool(QStringLiteral("pdfunite"))).isExecutable()) {
            result.errors.append(QStringLiteral("A união de PDFs precisa do utilitário pdfunite, que não está disponível."));
            return result;
        }
        QTemporaryDir temporary(directory.absoluteFilePath(QStringLiteral(".mandarina-XXXXXX")));
        if (!temporary.isValid()) {
            result.errors.append(QStringLiteral("Não foi possível criar um arquivo temporário na pasta de destino."));
            return result;
        }
        const QString staged = temporary.filePath(QStringLiteral("saida.pdf"));
        report(progress, 0, inputs.size(), inputs.size() > 1 ? QStringLiteral("Unindo PDFs") : QStringLiteral("Criando uma cópia do PDF"));
        ProcessResult status;
        if (inputs.size() == 1) {
            status = runProcess(RuntimePaths::tool(QStringLiteral("pdfinfo")), {inputs.first()}, cancel);
            if (status == ProcessResult::Success && !QFile::copy(inputs.first(), staged)) status = ProcessResult::Failure;
        } else {
            QStringList arguments = inputs;
            arguments.append(staged);
            status = runProcess(RuntimePaths::tool(QStringLiteral("pdfunite")), arguments, cancel);
        }
        if (status == ProcessResult::Cancelled || cancel.load()) result.cancelled = true;
        else if (status != ProcessResult::Success) result.errors.append(QStringLiteral("Não foi possível processar os PDFs; verifique se estão válidos, desbloqueados e permitem essa operação."));
        else {
            const QString published = publishFile(staged, directory, outputBase(inputs.first()), QStringLiteral("pdf"));
            if (published.isEmpty()) result.errors.append(QStringLiteral("Não foi possível salvar o PDF sem substituir um arquivo existente."));
            else result.outputs.append(published);
            report(progress, inputs.size(), inputs.size(), QStringLiteral("PDF concluído"));
        }
        return result;
    }

    Documents::Session documentSession;
    for (int index = 0; index < inputs.size(); ++index) {
        if (cancel.load()) { result.cancelled = true; break; }
        report(progress, index, inputs.size(), QStringLiteral("Convertendo arquivo %1 de %2").arg(index + 1).arg(inputs.size()));
        const QString &input = inputs.at(index);
        const QString &kind = kinds.at(index);
        const QDir fileDirectory(request.outputDir.trimmed().isEmpty()
                                     ? QFileInfo(input).absolutePath() : destination);
        if (kind == QStringLiteral("unsupported")) {
            appendError(result, index, QStringLiteral("Tipo não compatível ou arquivo sem permissão de leitura."));
            continue;
        }
        // Native image encoding and PDF rendering have no dependency on the
        // Python/OCR/Office capability probe, including on a cold first job.
        const bool nativeImage = kind == QStringLiteral("image") && !writerFormat(format).isEmpty();
        const bool nativePdfRender = kind == QStringLiteral("pdf")
                                     && (format == QStringLiteral("png") || format == QStringLiteral("jpg"));
        if (!nativeImage && !nativePdfRender && !outputFormats(kind).contains(format)) {
            appendError(result, index, QStringLiteral("Esse formato de saída não está disponível para este tipo de arquivo nesta instalação."));
            continue;
        }
        QString temporaryParent = fileDirectory.absolutePath();
#ifdef Q_OS_WIN
        if (kind == QStringLiteral("document")) {
            // Writer's extension registry still has MAX_PATH limits. Keep its
            // complete private tree short and owned by this C++ scope so even
            // a hard worker cancellation removes the copied source/profile.
            const QString localData = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
            temporaryParent = localData.isEmpty() ? QString{} : QDir(localData).filePath(QStringLiteral("Temp"));
            if (temporaryParent.isEmpty() || !QDir().mkpath(temporaryParent)) {
                appendError(result, index, QStringLiteral("Não foi possível criar a pasta temporária privada do documento."));
                continue;
            }
        }
#endif
        QTemporaryDir temporary(QDir(temporaryParent).absoluteFilePath(QStringLiteral(".mandarina-XXXXXX")));
        if (!temporary.isValid()) {
            appendError(result, index, QStringLiteral("Não foi possível criar um arquivo temporário para a conversão."));
            continue;
        }
        const QString staged = temporary.filePath(QStringLiteral("saida.") + format);
        const auto documentProgress = [&](int done, int total, const QString &phase) {
            const int units = 1000;
            report(progress, index * units + done * units / std::max(1, total), inputs.size() * units,
                (phase == QStringLiteral("ocr") ? QStringLiteral("Reconhecendo texto — página %1 de %2")
                                               : QStringLiteral("Extraindo texto — página %1 de %2"))
                    .arg(std::min(done + 1, total)).arg(total));
        };
        QString renderInput = input;
        if (kind == QStringLiteral("audio")) {
            const auto outcome = Audio::convert(input, format, staged, cancel);
            if (outcome.status == Audio::Status::Cancelled) { result.cancelled = true; break; }
            if (outcome.status != Audio::Status::Success) { appendError(result, index, outcome.error); continue; }
        } else if (kind == QStringLiteral("document")
                   || (kind == QStringLiteral("pdf") && format != QStringLiteral("png")
                       && format != QStringLiteral("jpg")
                       && (format != QStringLiteral("txt") || Documents::pdfOutputFormats().contains(format)))) {
            const bool renderPages = format == QStringLiteral("png") || format == QStringLiteral("jpg");
            if (renderPages) renderInput = temporary.filePath(QStringLiteral("documento.pdf"));
            const auto outcome = Documents::convert(input, renderPages ? QStringLiteral("pdf") : format,
                                                     renderPages ? renderInput : staged, cancel, documentProgress, &documentSession);
            if (outcome.status == Documents::Outcome::Cancelled) { result.cancelled = true; break; }
            if (outcome.status != Documents::Outcome::Success) { appendError(result, index, outcome.error); continue; }
        } else if (kind == QStringLiteral("image")
                   && (format == QStringLiteral("txt") || format == QStringLiteral("docx"))) {
            const QString prepared = temporary.filePath(QStringLiteral("ocr-source.png"));
            {
                QString error;
                QImage image = readImage(input, error);
                if (image.isNull()) { appendError(result, index, error); continue; }
                image = flattenOnWhite(image);
                if (image.width() > 4000 || image.height() > 4000)
                    image = image.scaled(4000, 4000, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                if (cancel.load()) { result.cancelled = true; break; }
                if (image.isNull() || !image.save(prepared, "PNG")) {
                    appendError(result, index, QStringLiteral("Não foi possível preparar esta imagem para reconhecer o texto."));
                    continue;
                }
            } // Release decoded image memory before starting the OCR worker.
            const auto outcome = Documents::convertImage(prepared, format, staged, cancel, documentProgress, &documentSession);
            if (outcome.status == Documents::Outcome::Cancelled) { result.cancelled = true; break; }
            if (outcome.status != Documents::Outcome::Success) { appendError(result, index, outcome.error); continue; }
        } else if (kind == QStringLiteral("image")) {
            QString error;
            QImage image = readImage(input, error);
            if (image.isNull()) { appendError(result, index, error); continue; }
            if (cancel.load()) { result.cancelled = true; break; }
            if (format == QStringLiteral("jpg")) image = flattenOnWhite(image);
            if (image.isNull()) {
                appendError(result, index, QStringLiteral("Não há memória suficiente para preparar a imagem."));
                continue;
            }
            QImageWriter writer(staged, writerFormat(format));
            writer.setQuality(std::clamp(request.quality, 1, 100));
            if (!writer.write(image)) {
                appendError(result, index, QStringLiteral("Não foi possível gravar nesse formato; verifique o suporte do sistema e o espaço na pasta de destino."));
                continue;
            }
        } else if (kind == QStringLiteral("pdf") && format == QStringLiteral("txt")) {
            const ProcessResult status = runProcess(RuntimePaths::tool(QStringLiteral("pdftotext")),
                {QStringLiteral("-enc"), QStringLiteral("UTF-8"), QStringLiteral("-layout"), input, staged}, cancel);
            if (status == ProcessResult::Cancelled) { result.cancelled = true; break; }
            if (status != ProcessResult::Success) {
                appendError(result, index, QStringLiteral("Não foi possível extrair o texto; o PDF pode estar inválido, protegido ou sem permissão."));
                continue;
            }
            QFile extracted(staged);
            if (!extracted.open(QIODevice::ReadOnly)) {
                appendError(result, index, QStringLiteral("Não foi possível ler o texto extraído na pasta de destino."));
                continue;
            }
            QTextStream text(&extracted);
            text.setEncoding(QStringConverter::Utf8);
            bool containsText = false;
            while (!text.atEnd() && !containsText && !cancel.load()) {
                const QString chunk = text.read(4096);
                containsText = std::any_of(chunk.cbegin(), chunk.cend(), [](QChar character) {
                    return !character.isSpace();
                });
            }
            extracted.close();
            if (cancel.load()) { result.cancelled = true; break; }
            if (!containsText) {
                appendError(result, index, QStringLiteral("Este PDF não contém texto extraível; esta versão ainda não faz OCR."));
                continue;
            }
        }
        if ((kind == QStringLiteral("pdf") || kind == QStringLiteral("document"))
            && (format == QStringLiteral("png") || format == QStringLiteral("jpg"))) {
            double renderDpi = 150;
            const ProcessResult preflight = pdfRenderDpi(renderInput, request.dpi, renderDpi, cancel);
            if (preflight == ProcessResult::Cancelled) { result.cancelled = true; break; }
            if (preflight != ProcessResult::Success) {
                appendError(result, index, QStringLiteral("Não foi possível validar as páginas do PDF; verifique se está válido e desbloqueado."));
                continue;
            }
            const QString pages = temporary.filePath(QStringLiteral("paginas"));
            if (!QDir().mkpath(pages)) {
                appendError(result, index, QStringLiteral("Não foi possível criar a pasta temporária de páginas."));
                continue;
            }
            QStringList arguments {QStringLiteral("-r"), QString::number(renderDpi, 'f', 10),
                                   QStringLiteral("-cropbox")};
            if (format == QStringLiteral("jpg")) {
                arguments << QStringLiteral("-jpeg") << QStringLiteral("-jpegopt")
                          << QStringLiteral("quality=%1").arg(std::clamp(request.quality, 1, 100));
            } else arguments << QStringLiteral("-png");
            arguments << renderInput << QDir(pages).absoluteFilePath(QStringLiteral("pagina"));
            const ProcessResult status = runProcess(RuntimePaths::tool(QStringLiteral("pdftoppm")), arguments, cancel);
            if (status == ProcessResult::Cancelled) { result.cancelled = true; break; }
            // Poppler pads page numbers to the same width, preserving page order.
            const QStringList pageFiles = QDir(pages).entryList(
                {QStringLiteral("pagina-*.") + format}, QDir::Files, QDir::Name);
            if (status != ProcessResult::Success || pageFiles.isEmpty()) {
                appendError(result, index, QStringLiteral("Não foi possível renderizar o PDF; verifique se está válido, desbloqueado e permite essa operação."));
                continue;
            }
            QStringList publishedPages;
            const QString base = outputBase(input);
            const int digits = QString::number(pageFiles.size()).size();
            for (int page = 0; page < pageFiles.size() && !cancel.load(); ++page) {
                const QString pageBase = pageFiles.size() == 1 ? base
                    : base + QStringLiteral("-pagina-%1").arg(page + 1, digits, 10, QLatin1Char('0'));
                const QString published = publishFile(QDir(pages).absoluteFilePath(pageFiles.at(page)),
                                                     fileDirectory, pageBase, format);
                if (published.isEmpty()) break;
                publishedPages.append(published);
            }
            if (cancel.load() || publishedPages.size() != pageFiles.size()) {
                // Roll back only files created for this document; retain earlier
                // conversions and every pre-existing destination file.
                for (const QString &published : publishedPages) QFile::remove(published);
                if (cancel.load()) { result.cancelled = true; break; }
                appendError(result, index, QStringLiteral("Não foi possível salvar todas as páginas sem substituir arquivos existentes."));
                continue;
            }
            result.outputs.append(publishedPages);
            report(progress, index + 1, inputs.size(), QStringLiteral("Páginas convertidas"));
            continue;
        }
        if (cancel.load()) { result.cancelled = true; break; }
        const QString published = publishFile(staged, fileDirectory, outputBase(input), format);
        if (published.isEmpty()) appendError(result, index, QStringLiteral("Não foi possível salvar o resultado sem substituir um arquivo existente."));
        else result.outputs.append(published);
        report(progress, index + 1, inputs.size(), QStringLiteral("Arquivo convertido"));
    }
    if (cancel.load()) result.cancelled = true;
    return result;
}

} // namespace Mandarina
