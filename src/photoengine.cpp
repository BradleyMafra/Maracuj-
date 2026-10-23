#include "photoengine.h"

#include <QColorSpace>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageIOHandler>
#include <QImageReader>
#include <QImageWriter>
#include <QPainter>
#include <QTemporaryDir>
#include <QtEndian>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace Mandarina::Photo {
namespace {
constexpr qint64 maximumPixels = 24LL * 1000 * 1000;
constexpr qint64 maximumBytes = 128LL * 1024 * 1024;
constexpr auto cancelledMessage = "Edição cancelada.";

bool stopped(std::atomic_bool &cancel) { return cancel.load(std::memory_order_relaxed); }
bool validSize(const QSize &size) {
    return size.width() > 0 && size.height() > 0
        && qint64(size.width()) * size.height() <= maximumPixels;
}

// Qt's PNG reader does not expose APNG frames on every supported Qt release.
// acTL must precede IDAT, so seek over ancillary chunks without reading their
// payloads. This also keeps header eligibility cheap for metadata-heavy PNGs.
bool staticPng(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.read(8) != QByteArray("\x89PNG\r\n\x1a\n", 8))
        return false;
    int chunks = 0;
    while (file.pos() + 12 <= file.size() && ++chunks <= 4096) {
        const QByteArray header = file.read(8);
        if (header.size() != 8) return false;
        const quint32 length = qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(header.constData()));
        const QByteArray type = header.mid(4, 4);
        if (type == "acTL") return false;
        if (qint64(length) + 4 > file.size() - file.pos()) return false;
        if (type == "IDAT") return true;
        if (type == "IEND") return false;
        if (!file.seek(file.pos() + qint64(length) + 4)) return false;
    }
    return false;
}

bool staticTiff(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QByteArray header = file.read(16);
    if (header.size() < 8 || (header.left(2) != "II" && header.left(2) != "MM")) return false;
    const bool little = header.left(2) == "II";
    const auto number = [little](const QByteArray &data, int width) -> quint64 {
        if (data.size() < width) return std::numeric_limits<quint64>::max();
        quint64 value = 0;
        for (int i = 0; i < width; ++i)
            value = (value << 8) | uchar(data[little ? width - 1 - i : i]);
        return value;
    };
    const quint64 magic = number(header.mid(2, 2), 2);
    const bool big = magic == 43;
    if (magic != 42 && !big) return false;
    if (big && (header.size() < 16 || number(header.mid(4, 2), 2) != 8 || number(header.mid(6, 2), 2) != 0)) return false;
    const int offsetBytes = big ? 8 : 4, countBytes = big ? 8 : 2, entryBytes = big ? 20 : 12;
    const quint64 offset = number(header.mid(big ? 8 : 4, offsetBytes), offsetBytes);
    if (offset < quint64(big ? 16 : 8) || offset > quint64(file.size())
        || quint64(file.size()) - offset < quint64(countBytes + offsetBytes) || !file.seek(qint64(offset))) return false;
    const quint64 entries = number(file.read(countBytes), countBytes);
    const quint64 available = quint64(file.size()) - offset - quint64(countBytes + offsetBytes);
    if (entries > available / quint64(entryBytes)) return false;
    if (!file.seek(qint64(offset + quint64(countBytes) + entries * quint64(entryBytes)))) return false;
    // A next-IFD link denotes another image/page, independently of whether the
    // installed Qt TIFF plugin exposes its page count after a size query.
    return number(file.read(offsetBytes), offsetBytes) == 0;
}

QString inspectHeader(const QString &path, QImageReader &reader) {
    const QFileInfo info(path);
    if (!info.isFile() || !info.isReadable())
        return QStringLiteral("Escolha uma imagem local que possa ser lida.");
    if (info.size() <= 0 || info.size() > maximumBytes)
        return QStringLiteral("A foto deve ter no máximo 128 MiB.");
    // Probe a separate device, then give the decoder its explicit content
    // format. Some Qt TIFF plugin versions leave their device at an IFD while
    // deciding the format, causing later size/page queries to reopen mid-file.
    QFile probe(info.absoluteFilePath());
    if (!probe.open(QIODevice::ReadOnly)) return QStringLiteral("Não foi possível ler esta imagem.");
    const QByteArray format = QImageReader::imageFormat(&probe).toLower();
    const QList<QByteArray> accepted{"png", "jpeg", "jpg", "webp", "bmp", "tiff", "tif", "heif", "heic", "avif", "jxl"};
    if (!accepted.contains(format))
        return QStringLiteral("O editor aceita fotos estáticas; este tipo de arquivo não é compatível.");
    if (((format == "tiff" || format == "tif") && !staticTiff(info.absoluteFilePath()))
        || (format == "png" && !staticPng(info.absoluteFilePath())))
        return QStringLiteral("Escolha uma foto estática, sem animação ou várias páginas.");
    reader.setFileName(info.absoluteFilePath());
    reader.setFormat(format);
    reader.setAutoTransform(true);
    if (!reader.canRead()) return QStringLiteral("Não foi possível reconhecer esta imagem.");
    if (!validSize(reader.size()))
        return QStringLiteral("O editor aceita fotos de até 24 megapixels.");
    if (reader.imageCount() > 1 || reader.supportsAnimation())
        return QStringLiteral("Escolha uma foto estática, sem animação ou várias páginas.");
    return {};
}

double parameter(const QVariantMap &values, const char *name, double minimum = -100, double maximum = 100) {
    bool ok = false;
    const double value = values.value(QString::fromLatin1(name)).toDouble(&ok);
    return ok && std::isfinite(value) ? std::clamp(value, minimum, maximum) : 0;
}

struct Settings {
    explicit Settings(const QVariantMap &values)
        : exposure(parameter(values, "exposure", -3, 3)), hue(parameter(values, "hue", -180, 180)) {
        constexpr std::array<const char *, 16> names{{"brightness", "contrast", "highlights", "shadows", "whites", "blacks",
            "gamma", "temperature", "tint", "saturation", "vibrance", "clarity", "dehaze", "sharpen", "denoise", "grain"}};
        for (size_t i = 0; i < valuesNormalized.size(); ++i)
            valuesNormalized[i] = parameter(values, names[i], i >= 13 ? 0 : -100, 100) / 100.;
    }
    enum Index { Brightness, Contrast, Highlights, Shadows, Whites, Blacks, Gamma,
                 Temperature, Tint, Saturation, Vibrance, Clarity, Dehaze, Sharpen, Denoise, Grain };
    double get(Index i) const { return valuesNormalized[size_t(i)]; }
    bool neutral() const {
        return exposure == 0 && hue == 0
            && std::all_of(valuesNormalized.begin(), valuesNormalized.end(), [](double value) { return value == 0; });
    }
    double exposure = 0, hue = 0;
    std::array<double, 16> valuesNormalized{};
};

int luminance(QRgb pixel) { return (54 * qRed(pixel) + 183 * qGreen(pixel) + 19 * qBlue(pixel) + 128) >> 8; }
int darkChannel(QRgb pixel) { return std::min({qRed(pixel), qGreen(pixel), qBlue(pixel)}); }
int byte(double value) { return qBound(0, qRound(value * 255), 255); }
double unit(double value) { return std::clamp(value, 0., 1.); }
double smoothstep(double minimum, double maximum, double value) {
    const double t = unit((value - minimum) / (maximum - minimum));
    return t * t * (3 - 2 * t);
}

// Alpha-weighted box means use two O(width) integer rows, rather than an
// integral image or a full-resolution floating-point buffer. Transparent
// padding cannot introduce dark halos around a cut-out photo.
QImage localMean(const QImage &source, int radius, bool useDarkChannel, std::atomic_bool &cancel) {
    QImage mean(source.size(), QImage::Format_Grayscale8);
    if (mean.isNull()) return {};
    const int width = source.width(), height = source.height();
    std::vector<qint64> columnValue(size_t(width), 0), columnWeight(size_t(width), 0);
    const auto addRow = [&](int row, int sign) {
        const auto *pixels = reinterpret_cast<const QRgb *>(source.constScanLine(qBound(0, row, height - 1)));
        for (int x = 0; x < width; ++x) {
            const QRgb pixel = pixels[x];
            const int weight = qAlpha(pixel);
            const int value = useDarkChannel ? darkChannel(pixel) : luminance(pixel);
            columnValue[size_t(x)] += qint64(sign) * value * weight;
            columnWeight[size_t(x)] += sign * weight;
        }
    };
    for (int row = -radius; row <= radius; ++row) {
        if (stopped(cancel)) return {};
        addRow(row, 1);
    }
    for (int y = 0; y < height; ++y) {
        if (stopped(cancel)) return {};
        auto *out = mean.scanLine(y);
        qint64 sum = 0, weight = 0;
        for (int x = -radius; x <= radius; ++x) {
            const int column = qBound(0, x, width - 1);
            sum += columnValue[size_t(column)];
            weight += columnWeight[size_t(column)];
        }
        for (int x = 0; x < width; ++x) {
            out[x] = uchar(weight ? (sum + weight / 2) / weight : (useDarkChannel ? 255 : 0));
            const int leaving = qBound(0, x - radius, width - 1);
            const int entering = qBound(0, x + radius + 1, width - 1);
            sum += columnValue[size_t(entering)] - columnValue[size_t(leaving)];
            weight += columnWeight[size_t(entering)] - columnWeight[size_t(leaving)];
        }
        addRow(y - radius, -1);
        addRow(y + radius + 1, 1);
    }
    return mean;
}

// A small bilateral filter mixes only neighbours of similar colour. It reduces
// sensor-like noise without the heavy edge blur of an ordinary box filter.
QImage denoised(const QImage &source, double amount, std::atomic_bool &cancel) {
    QImage result(source.size(), QImage::Format_ARGB32);
    if (result.isNull()) return {};
    std::array<double, 256> range{};
    const double sigma = 5 + amount * 32;
    for (size_t i = 0; i < range.size(); ++i) range[i] = std::exp(-double(i * i) / (2 * sigma * sigma));
    for (int y = 0; y < source.height(); ++y) {
        if (stopped(cancel)) return {};
        const auto *center = reinterpret_cast<const QRgb *>(source.constScanLine(y));
        auto *out = reinterpret_cast<QRgb *>(result.scanLine(y));
        for (int x = 0; x < source.width(); ++x) {
            const QRgb pixel = center[x];
            if (!qAlpha(pixel)) { out[x] = pixel; continue; }
            double r = 0, g = 0, b = 0, total = 0;
            for (int dy = -1; dy <= 1; ++dy) {
                const auto *row = reinterpret_cast<const QRgb *>(source.constScanLine(qBound(0, y + dy, source.height() - 1)));
                for (int dx = -1; dx <= 1; ++dx) {
                    const QRgb neighbour = row[qBound(0, x + dx, source.width() - 1)];
                    const int distance = std::max({std::abs(qRed(neighbour) - qRed(pixel)),
                        std::abs(qGreen(neighbour) - qGreen(pixel)), std::abs(qBlue(neighbour) - qBlue(pixel))});
                    const double spatial = (dx == 0 ? 2 : 1) * (dy == 0 ? 2 : 1);
                    const double weight = spatial * range[size_t(distance)] * qAlpha(neighbour);
                    r += qRed(neighbour) * weight; g += qGreen(neighbour) * weight; b += qBlue(neighbour) * weight;
                    total += weight;
                }
            }
            out[x] = qRgba(qRound(qRed(pixel) * (1 - amount) + r / total * amount),
                qRound(qGreen(pixel) * (1 - amount) + g / total * amount),
                qRound(qBlue(pixel) * (1 - amount) + b / total * amount), qAlpha(pixel));
        }
    }
    return result;
}

double atmosphere(const QImage &source, std::atomic_bool &cancel) {
    std::array<quint64, 256> histogram{};
    quint64 total = 0;
    // Sampling bounds this global estimate independently of the export size.
    const int step = qMax(1, qMin(source.width(), source.height()) / 512);
    for (int y = 0; y < source.height(); y += step) {
        if (stopped(cancel)) return 1;
        const auto *row = reinterpret_cast<const QRgb *>(source.constScanLine(y));
        for (int x = 0; x < source.width(); x += step) {
            const quint64 weight = quint64(qAlpha(row[x]));
            histogram[size_t(luminance(row[x]))] += weight;
            total += weight;
        }
    }
    quint64 count = 0;
    for (int i = 0; i < 256; ++i) {
        count += histogram[size_t(i)];
        if (count >= total * .995) return std::max(.70, i / 255.);
    }
    return 1;
}

quint32 mix(quint32 value) {
    value ^= value >> 16; value *= 0x7feb352dU;
    value ^= value >> 15; value *= 0x846ca68bU;
    return value ^ (value >> 16);
}
double grainAt(int x, int y) {
    const quint32 seed = mix(quint32(x) * 0x9e3779b9U ^ quint32(y) * 0x85ebca6bU ^ 0x4d617261U);
    // Sum of two uniforms gives quiet, repeatable monochromatic grain.
    return ((seed & 0xffffU) + (seed >> 16)) / 65535. - 1;
}

QString normalizedFormat(QString format) {
    format = format.trimmed().toLower();
    if (format == QStringLiteral("jpeg")) format = QStringLiteral("jpg");
    return format;
}

QString safeStem(const QString &path) {
    QString stem = QFileInfo(path).completeBaseName();
    for (QChar &character : stem) {
        if (character.unicode() < 32 || QStringLiteral("\\/:*?\"<>|").contains(character)) character = QLatin1Char('_');
    }
    stem = stem.left(96).trimmed();
    if (!stem.isEmpty() && stem.back().isHighSurrogate()) stem.chop(1);
    while (stem.endsWith(QLatin1Char('.')) || stem.endsWith(QLatin1Char(' '))) stem.chop(1);
    return stem.isEmpty() ? QStringLiteral("foto") : stem;
}

QSize boundedSize(const QSize &size, int edge) {
    const double scale = double(edge) / qMax(size.width(), size.height());
    return {qMax(1, qRound(size.width() * scale)), qMax(1, qRound(size.height() * scale))};
}
} // namespace

bool supportsPath(const QString &path) {
    QImageReader reader;
    return inspectHeader(path, reader).isEmpty();
}

Loaded loadImage(const QString &path, int maxEdge, std::atomic_bool &cancel) {
    Loaded result;
    if (stopped(cancel)) { result.error = QString::fromUtf8(cancelledMessage); return result; }
    QImageReader reader;
    result.error = inspectHeader(path, reader);
    if (!result.error.isEmpty()) return result;
    const QSize rawSize = reader.size();
    result.originalSize = rawSize;
    if (reader.transformation().testFlag(QImageIOHandler::TransformationRotate90))
        result.originalSize.transpose();
    if (maxEdge > 0 && qMax(rawSize.width(), rawSize.height()) > maxEdge)
        reader.setScaledSize(boundedSize(rawSize, maxEdge));
    if (stopped(cancel)) { result.error = QString::fromUtf8(cancelledMessage); return result; }
    QImage image = reader.read();
    if (stopped(cancel)) { result.error = QString::fromUtf8(cancelledMessage); return result; }
    if (image.isNull()) {
        result.error = QStringLiteral("Não foi possível abrir a foto: %1").arg(reader.errorString());
        return result;
    }
    if (!validSize(image.size()) || image.sizeInBytes() > maximumBytes) {
        result.error = QStringLiteral("A imagem decodificada excede o limite do editor."); return result;
    }
    if (reader.jumpToNextImage()) {
        result.error = QStringLiteral("O editor não altera imagens com várias páginas ou quadros."); return result;
    }
    if (maxEdge > 0 && qMax(image.width(), image.height()) > maxEdge)
        image = image.scaled(boundedSize(image.size(), maxEdge), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    if (image.colorSpace().isValid() && image.colorSpace() != QColorSpace(QColorSpace::SRgb))
        image.convertToColorSpace(QColorSpace(QColorSpace::SRgb));
    image = image.convertToFormat(QImage::Format_ARGB32);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    if (image.isNull()) result.error = QStringLiteral("Não há memória suficiente para abrir a foto.");
    else if (stopped(cancel)) result.error = QString::fromUtf8(cancelledMessage);
    else result.image = image;
    return result;
}

QImage apply(const QImage &source, const QVariantMap &adjustments, std::atomic_bool &cancel, QString &error) {
    error.clear();
    if (stopped(cancel)) { error = QString::fromUtf8(cancelledMessage); return {}; }
    if (source.isNull() || !validSize(source.size()) || source.sizeInBytes() > maximumBytes) {
        error = QStringLiteral("Escolha uma foto válida de até 24 megapixels."); return {};
    }
    QImage base = source;
    if (base.colorSpace().isValid() && base.colorSpace() != QColorSpace(QColorSpace::SRgb))
        base.convertToColorSpace(QColorSpace(QColorSpace::SRgb));
    base = base.convertToFormat(QImage::Format_ARGB32);
    base.setColorSpace(QColorSpace(QColorSpace::SRgb));
    if (base.isNull()) { error = QStringLiteral("Não há memória suficiente para editar a foto."); return {}; }
    const Settings settings(adjustments);
    if (settings.neutral()) return base;
    if (settings.get(Settings::Denoise) > 0) base = denoised(base, settings.get(Settings::Denoise), cancel);
    const auto failed = [&]() {
        error = stopped(cancel) ? QString::fromUtf8(cancelledMessage) : QStringLiteral("Não há memória suficiente para editar a foto.");
        return QImage{};
    };
    if (base.isNull() || stopped(cancel)) return failed();
    QImage broadMean, fineMean, darkMean;
    if (settings.get(Settings::Clarity) != 0) {
        const int radius = qBound(2, qRound(qMin(base.width(), base.height()) / 100.), 48);
        broadMean = localMean(base, radius, false, cancel);
        if (broadMean.isNull()) return failed();
    }
    if (settings.get(Settings::Sharpen) != 0) {
        fineMean = localMean(base, 1, false, cancel);
        if (fineMean.isNull()) return failed();
    }
    const double dehaze = settings.get(Settings::Dehaze);
    double air = 1;
    if (dehaze != 0) air = atmosphere(base, cancel);
    if (dehaze > 0) {
        const int radius = qBound(1, qRound(qMin(base.width(), base.height()) / 150.), 24);
        darkMean = localMean(base, radius, true, cancel);
        if (darkMean.isNull()) return failed();
    }
    if (stopped(cancel)) return failed();
    QImage result(base.size(), QImage::Format_ARGB32);
    if (result.isNull()) return failed();
    result.setColorSpace(QColorSpace(QColorSpace::SRgb));
    const double exposure = std::pow(2., settings.exposure);
    const double contrast = std::pow(2., 2 * settings.get(Settings::Contrast));
    const double gamma = std::pow(2., -settings.get(Settings::Gamma));
    const double angle = settings.hue * (3.14159265358979323846 / 180.);
    const double cosine = std::cos(angle), sine = std::sin(angle);
    for (int y = 0; y < base.height(); ++y) {
        if (stopped(cancel)) return failed();
        const auto *in = reinterpret_cast<const QRgb *>(base.constScanLine(y));
        auto *out = reinterpret_cast<QRgb *>(result.scanLine(y));
        const uchar *broad = broadMean.isNull() ? nullptr : broadMean.constScanLine(y);
        const uchar *fine = fineMean.isNull() ? nullptr : fineMean.constScanLine(y);
        const uchar *dark = darkMean.isNull() ? nullptr : darkMean.constScanLine(y);
        for (int x = 0; x < base.width(); ++x) {
            const QRgb pixel = in[x];
            if (!qAlpha(pixel)) { out[x] = pixel; continue; }
            double r = qRed(pixel) / 255., g = qGreen(pixel) / 255., b = qBlue(pixel) / 255.;
            if (dehaze > 0) {
                // Mild dark-channel atmospheric reconstruction; a conservative
                // transmission floor avoids extreme colour/noise amplification.
                const double transmission = std::max(.30, 1 - .75 * dehaze * unit(dark[x] / (255. * air)));
                r = (r - air) / transmission + air;
                g = (g - air) / transmission + air;
                b = (b - air) / transmission + air;
            } else if (dehaze < 0) {
                const double veil = -.45 * dehaze;
                r = r * (1 - veil) + air * veil;
                g = g * (1 - veil) + air * veil;
                b = b * (1 - veil) + air * veil;
            }
            const double originalLuma = luminance(pixel) / 255.;
            double detail = 0;
            if (broad) detail += settings.get(Settings::Clarity) * .85 * (originalLuma - broad[x] / 255.);
            if (fine) detail += settings.get(Settings::Sharpen) * 1.1 * (originalLuma - fine[x] / 255.);
            r += detail; g += detail; b += detail;
            const double light = unit((.2126 * r + .7152 * g + .0722 * b) * exposure);
            const double lift = .25 * settings.get(Settings::Brightness)
                + .35 * settings.get(Settings::Highlights) * smoothstep(.45, 1, light)
                + .35 * settings.get(Settings::Shadows) * (1 - smoothstep(0, .6, light))
                + .28 * settings.get(Settings::Whites) * light * light * light
                + .28 * settings.get(Settings::Blacks) * std::pow(1 - light, 3);
            r = (r * exposure + lift - .5) * contrast + .5;
            g = (g * exposure + lift - .5) * contrast + .5;
            b = (b * exposure + lift - .5) * contrast + .5;
            if (gamma != 1) { r = std::pow(unit(r), gamma); g = std::pow(unit(g), gamma); b = std::pow(unit(b), gamma); }
            const double temperature = settings.get(Settings::Temperature) * .12;
            const double tint = settings.get(Settings::Tint) * .10;
            r += temperature + tint * .35; g -= tint; b -= temperature - tint * .35;
            if (settings.hue != 0) {
                const double luminanceY = .299 * r + .587 * g + .114 * b;
                const double chromaI = .596 * r - .274 * g - .322 * b;
                const double chromaQ = .211 * r - .523 * g + .312 * b;
                const double rotatedI = chromaI * cosine - chromaQ * sine;
                const double rotatedQ = chromaI * sine + chromaQ * cosine;
                r = luminanceY + .956 * rotatedI + .621 * rotatedQ;
                g = luminanceY - .272 * rotatedI - .647 * rotatedQ;
                b = luminanceY - 1.106 * rotatedI + 1.703 * rotatedQ;
            }
            const double luma = .2126 * r + .7152 * g + .0722 * b;
            const double chroma = unit(std::max({r, g, b}) - std::min({r, g, b}));
            const double saturation = (1 + settings.get(Settings::Saturation))
                * (1 + settings.get(Settings::Vibrance) * (1 - chroma));
            r = luma + (r - luma) * saturation;
            g = luma + (g - luma) * saturation;
            b = luma + (b - luma) * saturation;
            const double grain = settings.get(Settings::Grain) * .12 * grainAt(x, y)
                * (.35 + .65 * std::sqrt(std::max(0., unit(luma) * (1 - unit(luma)) * 4)));
            out[x] = qRgba(byte(r + grain), byte(g + grain), byte(b + grain), qAlpha(pixel));
        }
    }
    return result;
}

QStringList outputFormats() {
    const auto available = QImageWriter::supportedImageFormats();
    QStringList formats;
    if (available.contains("png")) formats.append(QStringLiteral("png"));
    if (available.contains("jpeg") || available.contains("jpg")) formats.append(QStringLiteral("jpg"));
    if (available.contains("webp")) formats.append(QStringLiteral("webp"));
    return formats;
}

ExportResult exportCopy(const QString &path, const QString &outputDirectory, const QString &format, int quality,
                        const QVariantMap &adjustments, std::atomic_bool &cancel) {
    ExportResult result;
    const auto cancelled = [&]() {
        result.cancelled = true;
        result.error = QString::fromUtf8(cancelledMessage);
        return result;
    };
    if (stopped(cancel)) return cancelled();
    const QString targetFormat = normalizedFormat(format);
    if (!outputFormats().contains(targetFormat)) {
        result.error = QStringLiteral("Escolha PNG, JPG ou WebP entre os formatos disponíveis."); return result;
    }
    const QFileInfo source(path);
    const QString destination = outputDirectory.isEmpty() ? source.absolutePath() : QFileInfo(outputDirectory).absoluteFilePath();
    const QFileInfo folder(destination);
    if (!folder.isDir() || !folder.isWritable()) {
        result.error = QStringLiteral("A pasta de destino não existe ou não permite gravar arquivos."); return result;
    }
    Loaded loaded = loadImage(path, 0, cancel);
    if (stopped(cancel)) return cancelled();
    if (!loaded.error.isEmpty()) { result.error = loaded.error; return result; }
    QImage edited = apply(loaded.image, adjustments, cancel, result.error);
    loaded.image = {}; // Release the decoded source before encoding the result.
    if (stopped(cancel)) return cancelled();
    if (edited.isNull()) return result;
    if (targetFormat == QStringLiteral("jpg") && edited.hasAlphaChannel()) {
        QImage flattened(edited.size(), QImage::Format_RGB32);
        if (flattened.isNull()) { result.error = QStringLiteral("Não há memória suficiente para salvar a foto."); return result; }
        flattened.fill(Qt::white);
        { QPainter painter(&flattened); painter.drawImage(0, 0, edited); }
        flattened.setColorSpace(QColorSpace(QColorSpace::SRgb));
        edited = flattened;
    }
    if (stopped(cancel)) return cancelled();
    QTemporaryDir staging(QDir(destination).filePath(QStringLiteral(".maracuja-edit-XXXXXX")));
    if (!staging.isValid()) { result.error = QStringLiteral("Não foi possível preparar uma cópia na pasta de destino."); return result; }
    const QString stagedPath = staging.filePath(QStringLiteral("foto.") + targetFormat);
    {
        // Close the writer's file before rename, including on Windows where
        // an open encoder handle can prevent moving the staged file.
        QImageWriter writer(stagedPath, targetFormat == QStringLiteral("jpg") ? QByteArray("jpeg") : targetFormat.toLatin1());
        writer.setQuality(qBound(1, quality, 100));
        if (!writer.write(edited)) {
            result.error = QStringLiteral("Não foi possível salvar a cópia: %1").arg(writer.errorString()); return result;
        }
    }
    if (stopped(cancel)) return cancelled();
    const QString stem = safeStem(path) + QStringLiteral("-editado");
    for (int number = 1; number <= 9999; ++number) {
        if (stopped(cancel)) return cancelled();
        const QString suffix = number == 1 ? QString{} : QStringLiteral(" (%1)").arg(number);
        const QString target = QDir(destination).filePath(stem + suffix + QLatin1Char('.') + targetFormat);
        const QFileInfo candidate(target);
        if (candidate.exists() || candidate.isSymLink()) continue;
        // QFile::rename refuses existing destinations. Staging on the same
        // filesystem keeps publication atomic, without overwriting a race or a
        // dangling symlink created after the existence check.
        if (QFile::rename(stagedPath, target)) { result.output = target; return result; }
        const QFileInfo raced(target);
        if (raced.exists() || raced.isSymLink()) continue;
        result.error = QStringLiteral("Não foi possível criar a cópia editada na pasta de destino."); return result;
    }
    result.error = QStringLiteral("Há muitas cópias com este nome; escolha outra pasta.");
    return result;
}
} // namespace Mandarina::Photo
