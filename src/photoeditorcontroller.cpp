#include "photoeditorcontroller.h"
#include "outputdirectory.h"
#include <QDesktopServices>
#include <QFileInfo>
#include <QReadWriteLock>
#include <QtConcurrent>
#include <algorithm>
#include <cmath>

struct PhotoPreviewStore {
    QReadWriteLock lock;
    QImage original, preview;
};

namespace {
class PhotoImageProvider final : public QQuickImageProvider {
public:
    explicit PhotoImageProvider(std::shared_ptr<PhotoPreviewStore> store)
        : QQuickImageProvider(QQuickImageProvider::Image), m_store(std::move(store)) {}
    QImage requestImage(const QString &id, QSize *size, const QSize &requested) override {
        QImage image;
        {
            QReadLocker lock(&m_store->lock);
            image = id.startsWith("original/") ? m_store->original : m_store->preview;
        }
        if (size) *size = image.size();
        if (!image.isNull() && requested.isValid() && !requested.isEmpty())
            return image.scaled(requested.boundedTo(QSize(1280, 1280)), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        return image;
    }
private:
    std::shared_ptr<PhotoPreviewStore> m_store;
};
}

PhotoEditorController::PhotoEditorController(QObject *parent)
    : QObject(parent), m_store(std::make_shared<PhotoPreviewStore>()) {
    m_adjustments = neutralAdjustments();
    m_savedAdjustments = m_adjustments;
    m_status = tr("Escolha uma foto. O original será preservado.");
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(90);
    connect(&m_debounce, &QTimer::timeout, this, &PhotoEditorController::startPreview);
    connect(&m_watcher, &QFutureWatcher<PhotoEditorWork>::finished, this, &PhotoEditorController::workFinished);
}

PhotoEditorController::~PhotoEditorController() {
    if (m_cancel) m_cancel->store(true);
    m_watcher.waitForFinished();
}

QQuickImageProvider *PhotoEditorController::createImageProvider() const { return new PhotoImageProvider(m_store); }
QString PhotoEditorController::fileName() const { return QFileInfo(m_input).fileName(); }
QUrl PhotoEditorController::previewSource() const {
    return ready() ? QUrl(QStringLiteral("image://photo/preview/%1").arg(m_revision)) : QUrl{};
}
QUrl PhotoEditorController::originalSource() const {
    return ready() ? QUrl(QStringLiteral("image://photo/original/%1").arg(m_originalRevision)) : QUrl{};
}

QVariantList PhotoEditorController::controls() const {
    QVariantList result;
    const auto add = [&result](const char *id, const QString &label, const char *group,
                              double minimum = -100, double maximum = 100, double step = 1, const QString &unit = {}) {
        result << QVariantMap{{"id", id}, {"label", label}, {"group", group}, {"min", minimum},
                              {"max", maximum}, {"step", step}, {"unit", unit}};
    };
    add("exposure", tr("Exposição"), "light", -3, 3, 0.05, "EV");
    add("brightness", tr("Brilho"), "light");
    add("contrast", tr("Contraste"), "light");
    add("highlights", tr("Realces"), "light");
    add("shadows", tr("Sombras"), "light");
    add("whites", tr("Brancos"), "light");
    add("blacks", tr("Pretos"), "light");
    add("gamma", tr("Gama"), "light");
    add("temperature", tr("Temperatura"), "color");
    add("tint", tr("Matiz verde / magenta"), "color");
    add("saturation", tr("Saturação"), "color");
    add("vibrance", tr("Vivacidade"), "color");
    add("hue", tr("Tonalidade"), "color", -180, 180, 1, "°");
    add("clarity", tr("Clareza"), "detail");
    add("dehaze", tr("Remover névoa"), "detail");
    add("sharpen", tr("Nitidez"), "detail", 0, 100);
    add("denoise", tr("Redução de ruído"), "detail", 0, 100);
    add("grain", tr("Granulação"), "detail", 0, 100);
    return result;
}

QVariantMap PhotoEditorController::neutralAdjustments() const {
    QVariantMap values;
    for (const auto &control : controls()) values[control.toMap().value("id").toString()] = 0.0;
    return values;
}

void PhotoEditorController::setOutputDirectory(const QString &directory) {
    m_directory = Mandarina::normalizedOutputDirectory(directory);
}
void PhotoEditorController::setExportFormat(const QString &format) {
    if (busy() || format == m_format || !exportFormats().contains(format)) return;
    m_format = format;
    emit changed();
}
void PhotoEditorController::setQuality(int quality) {
    if (busy()) return;
    m_quality = qBound(10, quality, 100);
    emit changed();
}
void PhotoEditorController::setExternalBusy(bool busy) {
    if (busy == m_externalBusy) return;
    m_externalBusy = busy;
    if (busy && m_running && m_kind == PhotoEditorWork::Preview && m_cancel) {
        m_cancel->store(true);
        m_previewWanted = true;
    }
    if (!busy && m_previewWanted) m_debounce.start();
    emit busyChanged(); emit changed();
}

bool PhotoEditorController::openFile(const QString &path) {
    if (busy()) return false;
    if (!Mandarina::Photo::supportsPath(path)) return false;
    const auto canonical = QFileInfo(path).canonicalFilePath();
    if (m_active && ready() && canonical == m_input) { emit panelRequested(); return true; }
    if (m_active && dirty()) {
        m_error = tr("Salve ou descarte os ajustes atuais antes de abrir outra foto.");
        emit changed(); emit panelRequested(); return false;
    }
    if (m_cancel) m_cancel->store(true);
    ++m_generation;
    m_active = true; m_loading = true; m_previewWanted = false;
    m_debounce.stop();
    m_input = canonical; m_pendingPath = canonical;
    m_original = {}; m_output.clear(); m_error.clear();
    m_adjustments = neutralAdjustments(); m_savedAdjustments = m_adjustments;
    m_status = tr("Abrindo a foto localmente…");
    publishPreview({}, true);
    emit busyChanged(); emit changed(); emit panelRequested();
    if (!m_running) loadPending();
    return true;
}

void PhotoEditorController::loadPending() {
    if (m_pendingPath.isEmpty() || m_running) return;
    const auto path = m_pendingPath;
    const auto generation = m_generation;
    m_pendingPath.clear();
    m_running = true; m_kind = PhotoEditorWork::Load;
    m_cancel = std::make_shared<std::atomic_bool>(false);
    const auto cancel = m_cancel;
    m_watcher.setFuture(QtConcurrent::run([path, generation, cancel] {
        PhotoEditorWork result; result.kind = PhotoEditorWork::Load; result.generation = generation;
        const auto loaded = Mandarina::Photo::loadImage(path, 1280, *cancel);
        result.image = loaded.image; result.error = loaded.error; result.cancelled = cancel->load();
        return result;
    }));
}

void PhotoEditorController::setAdjustment(const QString &id, double value) {
    if (!ready() || busy() || !std::isfinite(value)) return;
    for (const auto &entry : controls()) {
        const auto control = entry.toMap();
        if (control.value("id").toString() != id) continue;
        value = std::clamp(value, control.value("min").toDouble(), control.value("max").toDouble());
        if (qFuzzyCompare(m_adjustments.value(id).toDouble() + 1, value + 1)) return;
        m_adjustments[id] = value;
        queuePreview();
        return;
    }
}
void PhotoEditorController::resetAdjustment(const QString &id) { setAdjustment(id, 0); }
void PhotoEditorController::resetAll() {
    if (!ready() || busy() || m_adjustments == neutralAdjustments()) return;
    m_adjustments = neutralAdjustments();
    queuePreview();
}
void PhotoEditorController::queuePreview() {
    ++m_generation;
    if (m_running && m_kind == PhotoEditorWork::Preview && m_cancel) m_cancel->store(true);
    m_previewWanted = true; m_output.clear(); m_error.clear();
    m_status = tr("Atualizando a prévia…");
    m_debounce.start();
    emit changed();
}
void PhotoEditorController::startPreview() {
    if (m_running || busy() || !m_active || !ready() || !m_previewWanted) return;
    m_previewWanted = false; m_running = true; m_kind = PhotoEditorWork::Preview;
    const auto image = m_original;
    const auto adjustments = m_adjustments;
    const auto generation = m_generation;
    m_cancel = std::make_shared<std::atomic_bool>(false);
    const auto cancel = m_cancel;
    m_watcher.setFuture(QtConcurrent::run([image, adjustments, generation, cancel] {
        PhotoEditorWork result; result.kind = PhotoEditorWork::Preview; result.generation = generation;
        result.adjustments = adjustments;
        result.image = Mandarina::Photo::apply(image, adjustments, *cancel, result.error);
        result.cancelled = cancel->load();
        return result;
    }));
    emit changed();
}

void PhotoEditorController::saveCopy() {
    if (busy() || rendering() || !m_active || !ready() || m_running) return;
    m_exporting = true; m_running = true; m_kind = PhotoEditorWork::Export;
    m_output.clear(); m_error.clear();
    m_status = tr("Salvando uma nova cópia na resolução original…");
    const auto path = m_input, directory = m_directory, format = m_format;
    const auto quality = m_quality;
    const auto adjustments = m_adjustments;
    const auto generation = m_generation;
    m_cancel = std::make_shared<std::atomic_bool>(false);
    const auto cancel = m_cancel;
    emit busyChanged(); emit changed();
    m_watcher.setFuture(QtConcurrent::run([path, directory, format, quality, adjustments, generation, cancel] {
        PhotoEditorWork result; result.kind = PhotoEditorWork::Export; result.generation = generation;
        result.adjustments = adjustments;
        const auto exported = Mandarina::Photo::exportCopy(path, directory, format, quality, adjustments, *cancel);
        result.output = exported.output; result.error = exported.error; result.cancelled = exported.cancelled;
        return result;
    }));
}

void PhotoEditorController::workFinished() {
    // Consume the image held by the future so a closed editor releases both
    // the provider cache and the last worker's decoded preview.
    const auto result = m_watcher.future().takeResult();
    m_running = false;
    if (result.generation == m_generation && m_active) {
        if (result.kind == PhotoEditorWork::Load) {
            m_loading = false;
            if (!result.cancelled && !result.image.isNull()) {
                m_original = result.image;
                publishPreview(result.image, true);
                m_status = tr("Ajuste a foto. Salvar cria uma nova cópia.");
            } else {
                m_error = result.error;
                m_status = result.cancelled ? tr("Abertura cancelada.") : tr("Não foi possível abrir esta foto.");
            }
        } else if (result.kind == PhotoEditorWork::Preview) {
            if (!result.cancelled && !result.image.isNull()) {
                publishPreview(result.image);
                m_status = tr("Prévia atualizada. O original permanece intacto.");
            } else if (!result.error.isEmpty() && !result.cancelled) m_error = result.error;
        } else {
            m_exporting = false;
            m_error = result.error; m_output = result.output;
            if (!m_output.isEmpty()) m_savedAdjustments = result.adjustments;
            m_status = result.cancelled ? tr("Exportação cancelada. O original foi preservado.")
                : m_output.isEmpty() ? tr("Não foi possível salvar a cópia.")
                : tr("Cópia editada salva. O original foi preservado.");
        }
    }
    if (!m_pendingPath.isEmpty()) loadPending();
    else if (m_previewWanted && !m_debounce.isActive()) startPreview();
    emit changed(); emit busyChanged();
}

void PhotoEditorController::publishPreview(const QImage &image, bool original) {
    {
        QWriteLocker lock(&m_store->lock);
        m_store->preview = image;
        if (original) m_store->original = image;
    }
    ++m_revision;
    if (original) ++m_originalRevision;
    emit previewChanged();
}
void PhotoEditorController::cancel() {
    if (m_cancel) m_cancel->store(true);
    m_previewWanted = false; m_debounce.stop();
    emit changed();
}
void PhotoEditorController::closeEditor() {
    if (busy()) return;
    cancel(); ++m_generation; m_active = false; m_pendingPath.clear();
    m_original = {}; m_output.clear(); m_error.clear(); m_input.clear();
    m_adjustments = neutralAdjustments(); m_savedAdjustments = m_adjustments;
    publishPreview({}, true);
    emit changed();
}
void PhotoEditorController::openOutputFolder() {
    if (!m_output.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(m_output).absolutePath()));
}
