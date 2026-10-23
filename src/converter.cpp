#include "converter.h"
#include "outputdirectory.h"
#include "documentconversion.h"
#include "audioconversion.h"
#include <QFileInfo>
#include <QDir>
#include <QUrl>
#include <QDesktopServices>
#include <QPointer>
#include <QtConcurrent>
#include <algorithm>

Converter::Converter(QObject *parent) : QObject(parent) {
    setStatus(tr("Escolha arquivos ou arraste para um formato."));
    connect(this, &Converter::filesChanged, this, &Converter::outputFolderChanged);
    connect(this, &Converter::outputDirectoryChanged, this, &Converter::outputFolderChanged);
    connect(&m_watcher, &QFutureWatcher<Mandarina::Result>::finished, this, [this] {
        const auto result = m_watcher.result();
        m_outputs = result.outputs;
        m_errors = result.errors;
        m_busy = false;
        if (result.cancelled) setStatus(tr("Conversão cancelada. Os originais foram preservados."));
        else if (m_errors.isEmpty()) {
            m_progress = 1;
            setStatus(tr("Pronto! %1 resultado(s) criado(s).").arg(m_outputs.size()));
        } else setStatus(m_outputs.isEmpty() ? tr("Não foi possível concluir a conversão.") : tr("Alguns arquivos foram convertidos. Confira os detalhes."));
        emit progressChanged();
        emit resultChanged();
        emit busyChanged();
    });
#ifdef Q_OS_WIN
    // Discover optional tools away from the GUI thread. Native image formats
    // remain available immediately, even if Python or FFmpeg is unavailable.
    m_formatPool.setMaxThreadCount(2);
    connect(&m_documentFormatsWatcher, &QFutureWatcher<void>::finished, this, [this] {
        m_documentFormatsLoading = false;
        if (m_busy) emit formatsChanged();
        else refreshFormats();
    });
    connect(&m_audioFormatsWatcher, &QFutureWatcher<QStringList>::finished, this, [this] {
        m_audioFormats = m_audioFormatsWatcher.result();
        m_audioFormatsLoading = false;
        if (m_busy) emit formatsChanged();
        else refreshFormats();
    });
    if (!Mandarina::Documents::capabilitiesReady()) {
        m_documentFormatsLoading = true;
        m_documentFormatsWatcher.setFuture(QtConcurrent::run(&m_formatPool, [] { Mandarina::Documents::prepareCapabilities(); }));
    }
    m_audioFormatsLoading = true;
    m_audioFormatsWatcher.setFuture(QtConcurrent::run(&m_formatPool, [] { return Mandarina::Audio::outputFormats(); }));
#endif
}

Converter::~Converter() {
    if (m_cancel) m_cancel->store(true);
    m_watcher.waitForFinished();
#ifdef Q_OS_WIN
    m_documentFormatsWatcher.waitForFinished();
    m_audioFormatsWatcher.waitForFinished();
#endif
}

bool Converter::formatsLoading() const {
#ifdef Q_OS_WIN
    return m_documentFormatsLoading || m_audioFormatsLoading;
#else
    return false;
#endif
}

QString Converter::kindFor(const QStringList &paths) const {
    QString kind;
    for (const auto &path : paths) {
        const auto current = Mandarina::sourceKind(path);
        if (current == "unsupported") return "unsupported";
        if (!kind.isEmpty() && current != kind) return "mixed";
        kind = current;
    }
    return kind.isEmpty() ? QStringLiteral("image") : kind;
}

QString Converter::inputKind() const { return kindFor(m_preview.isEmpty() ? m_files : m_preview); }

QVariantList Converter::files() const {
    QVariantList result;
    for (const auto &path : m_files) {
        const QFileInfo info(path);
        result << QVariantMap{{"name", info.fileName()}, {"path", path},
                              {"url", QUrl::fromLocalFile(path)}, {"kind", Mandarina::sourceKind(path)},
                              {"size", info.size() < 1024 * 1024 ? tr("%1 KB").arg(info.size() / 1024.0, 0, 'f', 0) : tr("%1 MB").arg(info.size() / 1048576.0, 0, 'f', 1)}};
    }
    return result;
}

QVariantList Converter::formats() const {
    return formatsForPaths(m_preview.isEmpty() ? m_files : m_preview);
}

QVariantList Converter::formatsForPaths(const QStringList &paths) const {
    QVariantList result;
    const auto kind = kindFor(paths);
    const auto add = [&result](const QString &id, const QString &label, const QString &detail) {
        result << QVariantMap{{"id", id}, {"label", label}, {"detail", detail}};
    };
    QStringList available;
#ifdef Q_OS_WIN
    if (kind == "image") {
        available = Mandarina::imageOutputFormats();
        available.append("pdf");
        available.append(Mandarina::Documents::cachedImageOcrFormats());
    } else if (kind == "pdf") {
        available = {"png", "jpg", "txt", "pdf"};
        available.append(Mandarina::Documents::cachedPdfOutputFormats());
        available.removeDuplicates();
    } else if (kind == "document") {
        available = Mandarina::Documents::cachedOutputFormats();
        if (available.contains("pdf")) available << "png" << "jpg";
    } else if (kind == "audio") {
        available = m_audioFormats;
    }
#else
    available = Mandarina::outputFormats(kind);
#endif
    if (kind == "pdf") {
        for (const auto &id : QStringList{"docx", "png", "jpg", "txt", "md", "html", "epub", "pptx", "xlsx", "pdf"}) {
            if (!available.contains(id)) continue;
            if (id == "pdf" && paths.size() < 2) continue;
            const auto detail = id == "png" || id == "jpg" ? tr("Páginas")
                : id == "pptx" ? tr("Páginas como imagens") : id == "xlsx" ? tr("Tabelas detectadas")
                : id == "pdf" ? tr("Unir PDFs") : id == "txt" ? tr("Texto · OCR automático") : tr("Texto reorganizado · OCR");
            add(id, id.toUpper(), detail);
        }
    } else if (kind == "image") {
        for (const auto &id : QStringList{"png", "jpg", "webp", "pdf", "tiff", "bmp", "avif", "heic", "txt", "docx"}) {
            if (available.contains(id)) add(id, id.toUpper(), id == "pdf" ? tr("Juntar fotos")
                : id == "txt" ? tr("Extrair texto · OCR") : id == "docx" ? tr("Word editável · OCR") : tr("Imagem"));
        }
    } else if (kind == "document") {
        for (const auto &id : QStringList{"pdf", "docx", "txt", "md", "html", "epub", "odt", "rtf", "png", "jpg"})
            if (available.contains(id)) add(id, id.toUpper(), id == "png" || id == "jpg" ? tr("Páginas") : tr("Documento"));
    } else if (kind == "audio") {
        for (const auto &id : available) add(id, id.toUpper(), tr("Áudio"));
    }
    return result;
}

void Converter::refreshFormats() {
    const auto available = formats();
    const bool found = std::any_of(available.cbegin(), available.cend(), [this](const QVariant &v) { return v.toMap().value("id").toString() == m_format; });
    if (!found && !available.isEmpty()) { m_format = available.first().toMap().value("id").toString(); emit selectedFormatChanged(); }
    emit formatsChanged();
}

void Converter::setSelectedFormat(const QString &format) {
    if (m_busy || m_format == format) return;
    for (const auto &entry : formats()) if (entry.toMap().value("id").toString() == format) {
        m_format = format; emit selectedFormatChanged(); return;
    }
}

void Converter::setOutputDirectory(const QString &directory) {
    if (m_busy) return;
    const auto path = Mandarina::normalizedOutputDirectory(directory);
    if (path == m_outputDirectory) return;
    m_outputDirectory = path;
    emit outputDirectoryChanged();
}

QUrl Converter::outputFolderUrl() const {
    const auto path = m_outputDirectory.isEmpty()
        ? (m_files.isEmpty() ? QString{} : QFileInfo(m_files.first()).absolutePath()) : m_outputDirectory;
    return path.isEmpty() ? QUrl{} : QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath());
}

QStringList Converter::parseUrls(const QVariantList &urls, QStringList *errors, bool allowUnsupported) const {
    QStringList result;
    for (const auto &variant : urls) {
        const QUrl url = variant.canConvert<QUrl>() ? variant.toUrl() : QUrl(variant.toString());
        QString path = url.isLocalFile() ? url.toLocalFile() : (url.scheme().isEmpty() ? variant.toString() : QString());
        const QFileInfo info(path);
        if (path.isEmpty() || !info.isFile() || !info.isReadable()) {
            if (errors) *errors << tr("Selecione arquivos locais que possam ser lidos.");
            continue;
        }
        path = info.canonicalFilePath();
        if (!allowUnsupported && Mandarina::sourceKind(path) == "unsupported") {
            if (errors) *errors << tr("%1: formato de imagem, PDF, documento ou áudio não reconhecido.").arg(info.fileName());
            continue;
        }
        if (!result.contains(path)) {
            if (result.size() >= 128) { if (errors) *errors << tr("Selecione até 128 arquivos por vez."); break; }
            result << path;
        }
    }
    return result;
}

void Converter::setInitialFiles(const QStringList &paths) {
    QVariantList urls;
    for (const auto &path : paths) urls << QUrl::fromUserInput(path, QDir::currentPath(), QUrl::AssumeLocalFile);
    addUrls(urls);
}

void Converter::addUrls(const QVariantList &urls) {
    appendUrls(urls, false);
}

void Converter::addEditingUrls(const QVariantList &urls) {
    appendUrls(urls, true);
}

bool Converter::canEditUrls(const QVariantList &urls) const {
    if (m_busy) return false;
    QStringList errors;
    const auto paths = parseUrls(urls, &errors, true);
    return !paths.isEmpty() && errors.isEmpty();
}

void Converter::appendUrls(const QVariantList &urls, bool editing) {
    if (m_busy) return;
    m_errors.clear(); m_outputs.clear();
    const auto parsed = parseUrls(urls, &m_errors, editing);
    for (const auto &path : parsed) if (!m_files.contains(path)) {
        if (m_files.size() >= 128) { m_errors << tr("A seleção chegou ao limite de 128 arquivos. Os demais não foram adicionados."); break; }
        m_files << path;
    }
    m_preview.clear();
    emit filesChanged(); refreshFormats(); emit resultChanged();
    if (editing) setStatus(m_files.isEmpty() ? tr("Escolha arquivos para começar.")
        : tr("Escolha uma ação para os arquivos selecionados. Os originais serão preservados."));
    else if (inputKind() == "mixed") setStatus(tr("Separe imagens, PDFs, documentos e áudios em conversões diferentes."));
    else setStatus(m_files.isEmpty() ? tr("Escolha arquivos para começar.") : tr("Clique em um formato ou solte os arquivos sobre ele."));
}

void Converter::removeFile(int index) {
    if (m_busy || index < 0 || index >= m_files.size()) return;
    m_files.removeAt(index); emit filesChanged(); refreshFormats();
}

void Converter::clearFiles() {
    if (m_busy) return;
    m_files.clear(); m_preview.clear(); m_outputs.clear(); m_errors.clear();
    emit filesChanged(); refreshFormats(); emit resultChanged();
    setStatus(tr("Escolha arquivos ou arraste para um formato."));
}

bool Converter::previewUrls(const QVariantList &urls) {
    if (m_busy) return false;
    const auto preview = parseUrls(urls);
    const auto kind = kindFor(preview);
    if (preview.isEmpty() || kind == "mixed" || kind == "unsupported") {
        clearDragPreview(); return false;
    }
    m_preview = preview;
    refreshFormats(); return !formats().isEmpty();
}

void Converter::clearDragPreview() { if (!m_preview.isEmpty()) { m_preview.clear(); refreshFormats(); } }

QVariantList Converter::formatsForUrls(const QVariantList &urls) const {
    if (m_busy) return {};
    QStringList errors;
    const auto paths = parseUrls(urls, &errors);
    return paths.isEmpty() || !errors.isEmpty() ? QVariantList{} : formatsForPaths(paths);
}

void Converter::convertUrls(const QVariantList &urls, const QString &format) {
    if (m_busy) return;
    QStringList errors;
    const auto paths = parseUrls(urls, &errors);
    const auto available = formatsForPaths(paths);
    const bool supported = std::any_of(available.cbegin(), available.cend(), [&format](const QVariant &entry) {
        return entry.toMap().value("id").toString() == format;
    });
    if (paths.isEmpty() || !errors.isEmpty() || !supported) {
        if (errors.isEmpty()) errors << tr("Confira os arquivos e o formato antes de converter.");
        m_errors = errors;
        m_outputs.clear();
        emit resultChanged();
        setStatus(tr("Confira os arquivos da seleção antes de converter."));
        return;
    }
    startRequest({paths, format, m_outputDirectory, qBound(10, m_quality, 100), qBound(72, m_dpi, 300)});
}

void Converter::dropOnFormat(const QVariantList &urls, const QString &format) {
    if (m_busy) return;
    QStringList errors;
    const auto paths = parseUrls(urls, &errors);
    m_files = paths; m_preview.clear(); m_errors = errors; m_outputs.clear();
    emit filesChanged(); refreshFormats(); emit resultChanged();
    if (!errors.isEmpty()) { setStatus(tr("Confira os arquivos da seleção antes de converter.")); return; }
    if (paths.isEmpty()) return;
    setSelectedFormat(format); start();
}

void Converter::convertTo(const QString &format) { setSelectedFormat(format); start(); }

void Converter::start() {
    if (m_busy || m_files.isEmpty()) return;
    if (formats().isEmpty()) {
        setStatus(formatsLoading() ? tr("Carregando os formatos disponíveis…")
                                  : tr("Separe os tipos de arquivos e confira os conversores disponíveis."));
        return;
    }
    startRequest({m_files, m_format, m_outputDirectory, qBound(10, m_quality, 100), qBound(72, m_dpi, 300)});
}

void Converter::startRequest(const Mandarina::Request &request) {
    m_errors.clear(); m_outputs.clear(); m_progress = 0; m_busy = true; ++m_job;
    emit resultChanged(); emit progressChanged(); emit busyChanged(); setStatus(tr("Preparando a conversão…"));
    m_cancel = std::make_shared<std::atomic_bool>(false);
    const auto token = m_cancel;
    const int job = m_job;
    const QPointer<Converter> self(this);
    m_watcher.setFuture(QtConcurrent::run([request, token, self, job] {
        return Mandarina::convert(request, *token, [self, job](int done, int total, const QString &message) {
            if (!self) return;
            QMetaObject::invokeMethod(self, [self, job, done, total, message] {
                if (!self || !self->m_busy || self->m_job != job) return;
                self->m_progress = total > 0 ? double(done) / total : 0;
                emit self->progressChanged(); self->setStatus(message);
            }, Qt::QueuedConnection);
        });
    }));
}

void Converter::cancel() { if (m_cancel) { m_cancel->store(true); setStatus(tr("Cancelando com segurança…")); } }
void Converter::setStatus(const QString &status) { m_status = status; emit statusChanged(); }

void Converter::openOutputFolder() {
    QString path = m_outputDirectory;
    if (!m_outputs.isEmpty()) { const QFileInfo file(m_outputs.first()); path = file.isDir() ? file.filePath() : file.absolutePath(); }
    else if (path.isEmpty() && !m_files.isEmpty()) path = QFileInfo(m_files.first()).absolutePath();
    if (!path.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}
