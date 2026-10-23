#include "conversionengine.h"
#include <QGuiApplication>
#include <QTemporaryDir>
#include <QPdfWriter>
#include <QPainter>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QFont>
#include <thread>
#include <chrono>
#include <cstdio>

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    QTemporaryDir temporary;
    if (!temporary.isValid()) return 2;
    const QString source = temporary.filePath("cancelar.pdf");
    {
        QPdfWriter pdf(source);
        pdf.setResolution(72);
        QPainter painter(&pdf);
        for (int i = 0; i < 100; ++i) {
            if (i > 0) pdf.newPage();
            painter.fillRect(30, 30, 300, 500, QColor("#d2b7f5"));
            painter.drawText(50, 100, QString::number(i));
        }
    }
    QFile original(source); if (!original.open(QIODevice::ReadOnly)) return 2; const auto originalBytes = original.readAll(); original.close();
    std::atomic_bool cancel{false};
    QElapsedTimer elapsed; elapsed.start();
    std::thread requestCancel([&cancel] { std::this_thread::sleep_for(std::chrono::milliseconds(150)); cancel.store(true); });
    const auto result = Mandarina::convert({{source}, "png", temporary.path(), 90, 150}, cancel, {});
    requestCancel.join();
    if (!original.open(QIODevice::ReadOnly)) return 2;
    const bool originalPreserved = original.readAll() == originalBytes;
    const auto leftovers = QDir(temporary.path()).entryList({".mandarina-*", "cancelar-convertido*"}, QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
    if (!result.cancelled || !result.outputs.isEmpty() || !originalPreserved || !leftovers.isEmpty() || elapsed.elapsed() > 5000) {
        std::fprintf(stderr, "Cancelamento ou limpeza falhou.\n"); return 1;
    }
    // Exercise a real document-helper child, beyond the existing Poppler path.
    if (!Mandarina::outputFormats("pdf").contains("pptx")) return 2;
    cancel.store(false);
    elapsed.restart();
    std::thread cancelDocument([&cancel] { std::this_thread::sleep_for(std::chrono::milliseconds(400)); cancel.store(true); });
    const auto document = Mandarina::convert({{source}, "pptx", temporary.path(), 90, 150}, cancel, {});
    cancelDocument.join();
    const auto documentLeftovers = QDir(temporary.path()).entryList({".mandarina-*", "cancelar-convertido*"}, QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
    if (!document.cancelled || !document.outputs.isEmpty() || !documentLeftovers.isEmpty() || elapsed.elapsed() > 5000) {
        std::fprintf(stderr, "Cancelamento do conversor de documentos ou limpeza falhou.\n"); return 1;
    }
    const QString ocrSource = temporary.filePath("cancelar-ocr.png");
    {
        QImage page(3400, 3400, QImage::Format_RGB32);
        page.fill(Qt::white);
        QPainter painter(&page);
        QFont font("DejaVu Sans"); font.setPixelSize(44); painter.setFont(font); painter.setPen(Qt::black);
        for (int line = 0; line < 45; ++line)
            painter.drawText(70, 80 + line * 70, QStringLiteral("Maracuja OCR cancelamento seguro 5729 linha %1").arg(line));
        painter.end();
        if (!page.save(ocrSource)) return 2;
    }
    cancel.store(false);
    elapsed.restart();
    std::thread cancelOcr;
    bool recognitionStarted = false;
    const auto ocr = Mandarina::convert({{ocrSource}, "docx", temporary.path(), 90, 150}, cancel,
        [&](int, int, const QString &message) {
            if (!recognitionStarted && message.startsWith(QStringLiteral("Reconhecendo texto"))) {
                recognitionStarted = true;
                cancelOcr = std::thread([&cancel] { std::this_thread::sleep_for(std::chrono::milliseconds(150)); cancel.store(true); });
            }
        });
    if (cancelOcr.joinable()) cancelOcr.join();
    const auto ocrLeftovers = QDir(temporary.path()).entryList({".mandarina-*", "cancelar-ocr-convertido*"}, QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
    if (!recognitionStarted || !ocr.cancelled || !ocr.outputs.isEmpty() || !ocrLeftovers.isEmpty() || elapsed.elapsed() > 5000) {
        std::fprintf(stderr, "Cancelamento ativo do OCR ou limpeza falhou.\n"); return 1;
    }
    std::puts("PASS: cancelamento de Poppler, documentos e OCR ativo, limpeza e originais preservados");
    return 0;
}
