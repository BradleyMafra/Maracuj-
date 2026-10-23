#include "photoengine.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtEndian>
#include <cmath>
#include <limits>
#include <thread>
#include <chrono>

using namespace Mandarina::Photo;
namespace {
int checks = 0, failures = 0;
void check(bool passed, const QString &message) {
    ++checks;
    if (!passed) { ++failures; QTextStream(stderr) << "FAIL: " << message << '\n'; }
}
QByteArray bytes(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}
bool write(const QString &path, const QByteArray &data) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}
QByteArray digest(const QString &path) { return QCryptographicHash::hash(bytes(path), QCryptographicHash::Sha256); }
bool samePixels(const QImage &a, const QImage &b) {
    if (a.size() != b.size() || a.isNull() || b.isNull()) return false;
    for (int y = 0; y < a.height(); ++y)
        for (int x = 0; x < a.width(); ++x)
            if (a.pixel(x, y) != b.pixel(x, y)) return false;
    return true;
}
double meanRed(const QImage &image, const QRect &area) {
    double sum = 0;
    for (int y = area.top(); y <= area.bottom(); ++y)
        for (int x = area.left(); x <= area.right(); ++x) sum += qRed(image.pixel(x, y));
    return sum / (area.width() * area.height());
}
double variance(const QImage &image, const QRect &area) {
    const double mean = meanRed(image, area);
    double sum = 0;
    for (int y = area.top(); y <= area.bottom(); ++y)
        for (int x = area.left(); x <= area.right(); ++x) sum += std::pow(qRed(image.pixel(x, y)) - mean, 2);
    return sum / (area.width() * area.height());
}
QImage adjusted(const QImage &source, const QVariantMap &parameters) {
    std::atomic_bool cancel{false};
    QString error;
    QImage result = apply(source, parameters, cancel, error);
    check(!result.isNull() && error.isEmpty(), QStringLiteral("adjustment succeeds: %1").arg(parameters.keys().join(QLatin1Char(','))));
    return result;
}
quint32 crc32(const QByteArray &data) {
    quint32 crc = 0xffffffffU;
    for (uchar value : data) {
        crc ^= value;
        for (int i = 0; i < 8; ++i) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320U : 0U);
    }
    return crc ^ 0xffffffffU;
}
QByteArray chunk(const QByteArray &type, const QByteArray &payload) {
    QByteArray result(4, '\0');
    qToBigEndian<quint32>(quint32(payload.size()), reinterpret_cast<uchar *>(result.data()));
    result += type + payload;
    QByteArray crc(4, '\0');
    qToBigEndian<quint32>(crc32(type + payload), reinterpret_cast<uchar *>(crc.data()));
    return result + crc;
}
QByteArray twoPageTiff() {
    // Two independent 1x1 RGB strips. Header-level TIFF page detection must
    // reject this file rather than quietly editing its first page.
    QByteArray result;
    QDataStream stream(&result, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.writeRawData("II", 2);
    stream << quint16(42) << quint32(8);
    const auto page = [&](quint32 strip, quint32 next) {
        stream << quint16(10);
        const auto entry = [&](quint16 tag, quint16 type, quint32 count, quint32 value) {
            stream << tag << type << count << value;
        };
        entry(256, 4, 1, 1); entry(257, 4, 1, 1); entry(258, 3, 3, 260);
        entry(259, 3, 1, 1); entry(262, 3, 1, 2); entry(273, 4, 1, strip);
        entry(277, 3, 1, 3); entry(278, 4, 1, 1); entry(279, 4, 1, 3); entry(284, 3, 1, 1);
        stream << next;
    };
    page(266, 134); page(269, 0);
    stream << quint16(8) << quint16(8) << quint16(8);
    stream.writeRawData("\x20\x40\x60\x90\xb0\xd0", 6);
    return result;
}
}

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    QTemporaryDir temporary;
    check(temporary.isValid(), QStringLiteral("private fixture directory"));
    std::atomic_bool cancel{false};
    QImage source(96, 64, QImage::Format_ARGB32);
    for (int y = 0; y < source.height(); ++y) {
        for (int x = 0; x < source.width(); ++x) {
            const int detail = (x / 3 + y / 3) % 2 ? 9 : -9;
            source.setPixel(x, y, qRgba(35 + x * 2 + detail, 40 + y * 3 + detail, 55 + x + y + detail,
                x < 12 ? 0 : x < 30 ? 128 : 255));
        }
    }
    const QImage before = source.copy();
    QString error;
    check(samePixels(apply(source, {}, cancel, error), source) && error.isEmpty(), QStringLiteral("neutral settings preserve every pixel and alpha"));
    const QStringList parameters{QStringLiteral("exposure"), QStringLiteral("brightness"), QStringLiteral("contrast"),
        QStringLiteral("highlights"), QStringLiteral("shadows"), QStringLiteral("whites"), QStringLiteral("blacks"),
        QStringLiteral("gamma"), QStringLiteral("temperature"), QStringLiteral("tint"), QStringLiteral("saturation"),
        QStringLiteral("vibrance"), QStringLiteral("hue"), QStringLiteral("clarity"), QStringLiteral("dehaze"),
        QStringLiteral("sharpen"), QStringLiteral("denoise"), QStringLiteral("grain")};
    QVariantMap all;
    for (const QString &name : parameters) {
        const double value = name == QStringLiteral("exposure") ? .7 : 65;
        const QImage changed = adjusted(source, {{name, value}});
        check(!samePixels(changed, source), QStringLiteral("%1 changes image pixels").arg(name));
        bool alphaPreserved = changed.size() == source.size();
        for (int y = 0; alphaPreserved && y < source.height(); ++y)
            for (int x = 0; alphaPreserved && x < source.width(); ++x)
                alphaPreserved = qAlpha(changed.pixel(x, y)) == qAlpha(source.pixel(x, y));
        check(alphaPreserved, QStringLiteral("%1 preserves exact alpha").arg(name));
        all.insert(name, value);
    }
    check(samePixels(source, before), QStringLiteral("processing never mutates the source image"));
    const QImage combined = adjusted(source, all);
    check(combined.size() == source.size(), QStringLiteral("combined adjustments retain pixel dimensions"));
    const auto ignored = adjusted(source, {{QStringLiteral("exposure"), std::numeric_limits<double>::infinity()},
        {QStringLiteral("contrast"), std::numeric_limits<double>::quiet_NaN()},
        {QStringLiteral("hue"), QStringLiteral("invalid")}, {QStringLiteral("unknown"), 100}});
    check(samePixels(ignored, source), QStringLiteral("non-finite, invalid and unknown parameters are neutral"));
    check(samePixels(adjusted(source, {{QStringLiteral("exposure"), 3}, {QStringLiteral("denoise"), 0}}),
                     adjusted(source, {{QStringLiteral("exposure"), 1e20}, {QStringLiteral("denoise"), -100}})),
          QStringLiteral("numeric parameters clamp to their documented limits"));

    QImage grey(16, 16, QImage::Format_ARGB32); grey.fill(qRgb(64, 64, 64));
    check(qRed(adjusted(grey, {{QStringLiteral("exposure"), 1}}).pixel(8, 8)) == 128,
          QStringLiteral("one exposure EV doubles a dark pixel"));
    check(qRed(adjusted(grey, {{QStringLiteral("brightness"), 50}}).pixel(8, 8)) > 64
              && qRed(adjusted(grey, {{QStringLiteral("gamma"), 50}}).pixel(8, 8)) > 64,
          QStringLiteral("positive brightness and gamma lift dark tones"));
    check(qRed(adjusted(grey, {{QStringLiteral("contrast"), 50}}).pixel(8, 8)) < 64,
          QStringLiteral("positive contrast deepens the darker half"));
    QImage tones(32, 16, QImage::Format_ARGB32);
    for (int y = 0; y < tones.height(); ++y)
        for (int x = 0; x < tones.width(); ++x) tones.setPixel(x, y, x < 16 ? qRgb(40, 40, 40) : qRgb(210, 210, 210));
    const QImage shadows = adjusted(tones, {{QStringLiteral("shadows"), 50}});
    const QImage highlights = adjusted(tones, {{QStringLiteral("highlights"), 50}});
    check(qRed(shadows.pixel(4, 4)) - 40 > qRed(shadows.pixel(24, 4)) - 210,
          QStringLiteral("shadows act more on dark than bright areas"));
    check(qRed(highlights.pixel(24, 4)) - 210 > qRed(highlights.pixel(4, 4)) - 40,
          QStringLiteral("highlights act more on bright than dark areas"));
    const QImage whites = adjusted(tones, {{QStringLiteral("whites"), 50}});
    const QImage blacks = adjusted(tones, {{QStringLiteral("blacks"), 50}});
    check(qRed(whites.pixel(24, 4)) - 210 > qRed(whites.pixel(4, 4)) - 40
              && qRed(blacks.pixel(4, 4)) - 40 > qRed(blacks.pixel(24, 4)) - 210,
          QStringLiteral("white and black point controls target their tonal ends"));
    QImage color(8, 8, QImage::Format_ARGB32); color.fill(qRgb(90, 120, 150));
    const QRgb warm = adjusted(color, {{QStringLiteral("temperature"), 50}}).pixel(3, 3);
    check(qRed(warm) > 90 && qBlue(warm) < 150, QStringLiteral("warm temperature moves red and blue in opposite directions"));
    check(qGreen(adjusted(color, {{QStringLiteral("tint"), 50}}).pixel(3, 3)) < 120,
          QStringLiteral("positive tint moves away from green"));
    const QRgb monochrome = adjusted(color, {{QStringLiteral("saturation"), -100}}).pixel(3, 3);
    check(qRed(monochrome) == qGreen(monochrome) && qGreen(monochrome) == qBlue(monochrome),
          QStringLiteral("minimum saturation produces a neutral monochrome result"));
    const QRgb vibrant = adjusted(color, {{QStringLiteral("vibrance"), 80}}).pixel(3, 3);
    check(qBlue(vibrant) - qRed(vibrant) > 60, QStringLiteral("vibrance increases muted colour separation"));

    QImage noisy(128, 64, QImage::Format_ARGB32);
    for (int y = 0; y < noisy.height(); ++y)
        for (int x = 0; x < noisy.width(); ++x) {
            const int value = (x < 64 ? 70 : 180) + ((x * 73 + y * 37) % 29 - 14);
            noisy.setPixel(x, y, qRgb(value, value, value));
        }
    const QImage cleaned = adjusted(noisy, {{QStringLiteral("denoise"), 100}});
    const QRect darkRegion(8, 8, 45, 45), brightRegion(74, 8, 45, 45);
    check(variance(cleaned, darkRegion) < variance(noisy, darkRegion) * .65,
          QStringLiteral("noise reduction lowers flat-region variance"));
    check(meanRed(cleaned, brightRegion) - meanRed(cleaned, darkRegion) > 100,
          QStringLiteral("noise reduction retains a strong neighbouring edge"));
    QImage flat(96, 64, QImage::Format_ARGB32); flat.fill(qRgb(128, 128, 128));
    const QImage grain = adjusted(flat, {{QStringLiteral("grain"), 100}});
    check(variance(grain, grain.rect()) > 20 && samePixels(grain, adjusted(flat, {{QStringLiteral("grain"), 100}})),
          QStringLiteral("grain has measurable variance and is deterministic across previews"));
    const QImage clear = adjusted(noisy, {{QStringLiteral("clarity"), 80}});
    const QImage sharpened = adjusted(noisy, {{QStringLiteral("sharpen"), 80}});
    check(variance(clear, darkRegion) > variance(noisy, darkRegion)
              && variance(sharpened, darkRegion) > variance(noisy, darkRegion),
          QStringLiteral("clarity and sharpening increase local detail contrast"));
    const QImage haze = adjusted(tones, {{QStringLiteral("dehaze"), -60}});
    const QImage removedHaze = adjusted(haze, {{QStringLiteral("dehaze"), 70}});
    check(qRed(haze.pixel(4, 4)) > 40
              && qRed(removedHaze.pixel(24, 4)) - qRed(removedHaze.pixel(4, 4))
                    > qRed(haze.pixel(24, 4)) - qRed(haze.pixel(4, 4)),
          QStringLiteral("dehaze recovers contrast from a veiled image"));

    const QString input = temporary.filePath(QStringLiteral("fóto.png"));
    check(source.save(input), QStringLiteral("generate source PNG"));
    const QByteArray originalDigest = digest(input);
    check(supportsPath(input), QStringLiteral("static PNG is eligible from its header"));
    const Loaded preview = loadImage(input, 32, cancel);
    check(preview.error.isEmpty() && !preview.image.isNull() && qMax(preview.image.width(), preview.image.height()) <= 32
              && preview.originalSize == source.size(), QStringLiteral("bounded preview reports original dimensions"));
    const Loaded full = loadImage(input, 0, cancel);
    check(full.error.isEmpty() && samePixels(full.image, source), QStringLiteral("full decode preserves pixel dimensions and alpha"));
    const QString disguised = temporary.filePath(QStringLiteral("photo.data"));
    check(write(disguised, bytes(input)) && supportsPath(disguised), QStringLiteral("eligibility uses content rather than filename suffix"));
    const QString invalid = temporary.filePath(QStringLiteral("invalid.png"));
    check(write(invalid, QByteArray("not an image")) && !supportsPath(invalid) && !loadImage(invalid, 64, cancel).error.isEmpty(),
          QStringLiteral("invalid input has an actionable error"));
    const QString svg = temporary.filePath(QStringLiteral("vector.svg"));
    check(write(svg, QByteArray("<svg xmlns='http://www.w3.org/2000/svg' width='10' height='10'><rect width='10' height='10'/></svg>"))
              && !supportsPath(svg), QStringLiteral("vector files are not offered as editable photos"));
    const QString pdf = temporary.filePath(QStringLiteral("document.pdf"));
    check(write(pdf, QByteArray("%PDF-1.4\n%%EOF")) && !supportsPath(pdf), QStringLiteral("PDF is not a photo"));
    QByteArray oversized = bytes(input);
    qToBigEndian<quint32>(6000, reinterpret_cast<uchar *>(oversized.data() + 16));
    qToBigEndian<quint32>(4001, reinterpret_cast<uchar *>(oversized.data() + 20));
    qToBigEndian<quint32>(crc32(oversized.mid(12, 17)), reinterpret_cast<uchar *>(oversized.data() + 29));
    const QString oversizedPath = temporary.filePath(QStringLiteral("large-header.png"));
    check(write(oversizedPath, oversized) && !supportsPath(oversizedPath)
              && !loadImage(oversizedPath, 32, cancel).error.isEmpty(), QStringLiteral("24 MP limit is enforced before preview decoding"));
    const QString largeFile = temporary.filePath(QStringLiteral("large-file.png"));
    { QFile file(largeFile); check(file.open(QIODevice::WriteOnly) && file.write(bytes(input)) > 0
              && file.resize(128LL * 1024 * 1024 + 1), QStringLiteral("generate sparse over-limit source")); }
    check(!supportsPath(largeFile), QStringLiteral("128 MiB encoded-file limit is enforced from metadata"));
    QFile::remove(largeFile);
    QByteArray apng = bytes(input);
    apng.insert(33, chunk("acTL", QByteArray::fromHex("0000000200000000")));
    const QString animated = temporary.filePath(QStringLiteral("animated.png"));
    check(write(animated, apng) && !supportsPath(animated), QStringLiteral("APNG rejected even when the Qt PNG plugin only exposes one frame"));
    if (QImageReader::supportedImageFormats().contains("tiff")) {
        const QString multi = temporary.filePath(QStringLiteral("two-pages.tiff"));
        check(write(multi, twoPageTiff()) && QImageReader(multi).imageCount() == 2, QStringLiteral("generate two-page TIFF fixture"));
        check(!supportsPath(multi), QStringLiteral("multipage TIFF is rejected during cheap tool eligibility"));
        check(!loadImage(multi, 64, cancel).error.isEmpty(), QStringLiteral("multipage images cannot silently lose pages"));
        const QString hiddenMulti = temporary.filePath(QStringLiteral("two-pages.data"));
        check(write(hiddenMulti, bytes(multi)) && !supportsPath(hiddenMulti),
              QStringLiteral("multipage rejection also follows TIFF content without its suffix"));
        if (QImageWriter::supportedImageFormats().contains("tiff")) {
            const QString onePage = temporary.filePath(QStringLiteral("one-page.tiff"));
            check(source.save(onePage, "TIFF") && supportsPath(onePage), QStringLiteral("single-page TIFF remains an eligible photo"));
            const Loaded one = loadImage(onePage, 48, cancel);
            check(one.error.isEmpty() && !one.image.isNull() && one.originalSize == source.size(),
                  QStringLiteral("single-page TIFF decodes safely with bounded preview"));
        }
    }

    const QString custom = temporary.filePath(QStringLiteral("cópias escolhidas"));
    check(QDir().mkpath(custom), QStringLiteral("create explicit export directory"));
    const ExportResult beside = exportCopy(input, {}, QStringLiteral("png"), 90, {}, cancel);
    check(beside.error.isEmpty() && !beside.output.isEmpty() && QFileInfo(beside.output).absolutePath() == temporary.path()
              && samePixels(QImage(beside.output), source), QStringLiteral("default export saves a lossless copy alongside the original"));
    const ExportResult explicitCopy = exportCopy(input, custom, QStringLiteral("png"), 90, {{QStringLiteral("exposure"), .5}}, cancel);
    check(explicitCopy.error.isEmpty() && QFileInfo(explicitCopy.output).absolutePath() == custom
              && QImage(explicitCopy.output).size() == source.size(), QStringLiteral("explicit destination retains full original dimensions"));
    const QByteArray firstCopyDigest = digest(beside.output);
    const ExportResult collision = exportCopy(input, {}, QStringLiteral("png"), 90, all, cancel);
    check(collision.error.isEmpty() && collision.output != beside.output && digest(beside.output) == firstCopyDigest,
          QStringLiteral("a same-name existing export is preserved"));
    const QString danglingSource = temporary.filePath(QStringLiteral("link.png"));
    check(write(danglingSource, bytes(input)), QStringLiteral("create source for symlink collision fixture"));
    const QString dangling = temporary.filePath(QStringLiteral("link-editado.png"));
#ifndef Q_OS_WIN
    const bool symlink = QFile::link(temporary.filePath(QStringLiteral("missing-target")), dangling);
    check(symlink && QFileInfo(dangling).isSymLink(), QStringLiteral("generate dangling destination symlink"));
    if (symlink) {
        const ExportResult safe = exportCopy(danglingSource, {}, QStringLiteral("png"), 90, {}, cancel);
        check(safe.error.isEmpty() && safe.output != dangling && QFileInfo(dangling).isSymLink(),
              QStringLiteral("dangling symlinks are never followed or replaced by export"));
    }
#endif
    check(!exportCopy(input, input, QStringLiteral("png"), 90, {}, cancel).error.isEmpty(),
          QStringLiteral("invalid configured directory fails without falling back"));
    check(!exportCopy(input, {}, QStringLiteral("gif"), 90, {}, cancel).error.isEmpty(),
          QStringLiteral("export refuses unavailable or unsupported formats"));
    if (outputFormats().contains(QStringLiteral("jpg"))) {
        const ExportResult jpeg = exportCopy(input, custom, QStringLiteral("jpeg"), 100, {}, cancel);
        QImage jpegImage(jpeg.output);
        check(jpeg.error.isEmpty() && jpegImage.size() == source.size() && supportsPath(jpeg.output),
              QStringLiteral("JPEG export retains dimensions and remains an eligible static photo"));
        check(!jpegImage.isNull() && qRed(jpegImage.pixel(3, 32)) > 245 && qGreen(jpegImage.pixel(3, 32)) > 245
                  && qBlue(jpegImage.pixel(3, 32)) > 245, QStringLiteral("JPEG flattens transparent pixels onto white"));
        const QString rotated = temporary.filePath(QStringLiteral("oriented.jpg"));
        QByteArray oriented = bytes(jpeg.output);
        oriented.insert(2, QByteArray::fromHex("ffe1002245786966000049492a0008000000010012010300010000000600000000000000"));
        check(write(rotated, oriented), QStringLiteral("generate EXIF orientation fixture"));
        const Loaded orientation = loadImage(rotated, 0, cancel);
        check(orientation.error.isEmpty() && orientation.image.size() == QSize(source.height(), source.width())
                  && orientation.originalSize == orientation.image.size(), QStringLiteral("EXIF rotation is normalized before editing"));
        const ExportResult orientedCopy = exportCopy(rotated, custom, QStringLiteral("png"), 90, {}, cancel);
        check(orientedCopy.error.isEmpty() && QImage(orientedCopy.output).size() == QSize(source.height(), source.width()),
              QStringLiteral("export preserves the normalized orientation without rotating twice"));
    }
    if (outputFormats().contains(QStringLiteral("webp"))) {
        const ExportResult webp = exportCopy(input, custom, QStringLiteral("webp"), 90, {}, cancel);
        check(webp.error.isEmpty() && QImage(webp.output).size() == source.size() && supportsPath(webp.output),
              QStringLiteral("available WebP export remains an editable static photo"));
    }
    cancel.store(true);
    check(!loadImage(input, 64, cancel).error.isEmpty(), QStringLiteral("pre-cancelled decode returns no image"));
    check(apply(source, all, cancel, error).isNull() && !error.isEmpty(), QStringLiteral("pre-cancelled adjustment returns no result"));
    const auto filesBefore = QDir(custom).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
    const ExportResult cancelled = exportCopy(input, custom, QStringLiteral("png"), 90, all, cancel);
    check(cancelled.cancelled && cancelled.output.isEmpty()
              && QDir(custom).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot) == filesBefore,
          QStringLiteral("pre-cancelled export leaves neither a copy nor staging files"));
    cancel.store(false);
    QImage large(1536, 1024, QImage::Format_ARGB32); large.fill(qRgb(100, 130, 160));
    std::thread cancelWorker([&] { std::this_thread::sleep_for(std::chrono::milliseconds(2)); cancel.store(true); });
    const QImage interrupted = apply(large, {{QStringLiteral("denoise"), 100}, {QStringLiteral("clarity"), 100}}, cancel, error);
    cancelWorker.join();
    check(interrupted.isNull() && !error.isEmpty(), QStringLiteral("cancellation interrupts active row-based processing"));
    check(digest(input) == originalDigest, QStringLiteral("all previews and exports preserve original file bytes"));
    check(QDir(custom).entryList({QStringLiteral(".maracuja-edit-*")}, QDir::Dirs | QDir::Hidden).isEmpty(),
          QStringLiteral("successful and failed operations clean their private staging directories"));
    QTextStream(stdout) << checks << " photo engine checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
