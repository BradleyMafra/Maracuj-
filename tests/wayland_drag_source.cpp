#include <QApplication>
#include <QColor>
#include <QDrag>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QTimer>
#include <QUrl>
#include <QWidget>
#include <cstdio>

namespace {
void printResult(const QString &action, int value, bool originalExists)
{
    const auto bytes = QJsonDocument(QJsonObject{
        {QStringLiteral("action"), action},
        {QStringLiteral("action_value"), value},
        {QStringLiteral("original_exists"), originalExists}
    }).toJson(QJsonDocument::Compact);
    std::fwrite(bytes.constData(), 1, bytes.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

class DragFixture final : public QWidget
{
public:
    DragFixture(const QString &path, const QImage &image)
        : m_path(path), m_image(image)
    {
        setWindowTitle(QStringLiteral("MandarinaDragFixture"));
        setFixedSize(400, 250);
        setMouseTracking(true);
        QTimer::singleShot(20000, this, [this] {
            m_timedOut = true;
            printResult(QStringLiteral("timeout"), int(Qt::IgnoreAction), QFileInfo::exists(m_path));
            // Cancels only this fixture's drag, then exits only this app.
            QDrag::cancel();
            QCoreApplication::exit(124);
        });
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillRect(rect(), QColor(QStringLiteral("#f6f0fc")));
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(QStringLiteral("#ffffff")));
        painter.drawRoundedRect(QRectF(18, 18, 364, 214), 18, 18);
        const QImage preview = m_image.scaled(96, 96, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        painter.drawImage(QPoint(30 + (96 - preview.width()) / 2,
                                 36 + (96 - preview.height()) / 2), preview);
        painter.setPen(QColor(QStringLiteral("#42334f")));
        QFont heading = painter.font();
        heading.setPointSize(13);
        heading.setBold(true);
        painter.setFont(heading);
        painter.drawText(QRect(142, 35, 220, 62), Qt::AlignLeft | Qt::TextWordWrap,
                         QStringLiteral("Arquivo de teste\ndo Mandarina"));
        QFont body = heading;
        body.setPointSize(10);
        body.setBold(false);
        painter.setFont(body);
        painter.drawText(QRect(30, 155, 330, 54), Qt::AlignLeft | Qt::TextWordWrap,
                         QStringLiteral("Arraste a partir de qualquer ponto.\nSomente este arquivo temporário será oferecido."));
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && !m_started) {
            m_pressPosition = event->position().toPoint();
            m_pressed = true;
            event->accept();
        } else {
            QWidget::mousePressEvent(event);
        }
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        m_pressed = false;
        QWidget::mouseReleaseEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!m_pressed || m_started || !(event->buttons() & Qt::LeftButton)
            || (event->position().toPoint() - m_pressPosition).manhattanLength() < 8) {
            QWidget::mouseMoveEvent(event);
            return;
        }
        m_pressed = false;
        m_started = true;
        QDrag drag(this);
        auto *mime = new QMimeData;
        mime->setUrls({QUrl::fromLocalFile(m_path)});
        drag.setMimeData(mime);
        const QPixmap pixmap = QPixmap::fromImage(m_image.scaled(72, 72, Qt::KeepAspectRatio,
                                                               Qt::SmoothTransformation));
        drag.setPixmap(pixmap);
        drag.setHotSpot(QPoint(pixmap.width() / 2, pixmap.height() / 2));
        const Qt::DropAction result = drag.exec(Qt::CopyAction | Qt::MoveAction, Qt::CopyAction);
        if (m_timedOut) return;
        const bool originalExists = QFileInfo::exists(m_path);
        const QString label = result == Qt::CopyAction ? QStringLiteral("copy")
                            : result == Qt::MoveAction ? QStringLiteral("move")
                            : result == Qt::LinkAction ? QStringLiteral("link")
                                                     : QStringLiteral("ignore");
        printResult(label, int(result), originalExists);
        QCoreApplication::exit(result == Qt::CopyAction && originalExists ? 0 : 1);
    }

private:
    QString m_path;
    QImage m_image;
    QPoint m_pressPosition;
    bool m_pressed = false;
    bool m_started = false;
    bool m_timedOut = false;
};
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("MandarinaDragFixture"));
    app.setDesktopFileName(QStringLiteral("org.winux.MandarinaDragFixture"));
    const auto arguments = app.arguments();
    if (arguments.size() != 2) {
        std::fprintf(stderr, "Uso: mandarina-wayland-drag-source /caminho/fixture.png\n");
        return 2;
    }
    const QFileInfo file(arguments.at(1));
    QImage image(file.absoluteFilePath());
    if (image.isNull() && file.isFile() && file.isReadable()) {
        image = QImage(80, 100, QImage::Format_RGB32); image.fill(QColor("#eddcf7"));
    }
    if (!file.isFile() || !file.isReadable() || image.isNull()) {
        std::fprintf(stderr, "O arquivo temporário de teste não pôde ser lido.\n");
        return 2;
    }
    DragFixture fixture(file.canonicalFilePath(), image);
    fixture.show();
    return app.exec();
}
