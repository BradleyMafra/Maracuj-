#include "filetoolscontroller.h"
#include "outputdirectory.h"
#include "photoengine.h"
#include <QFileInfo>
#include <QUrl>
#include <QDesktopServices>
#include <QPointer>
#include <QtConcurrent>

FileToolsController::FileToolsController(QObject *parent) : QObject(parent)
{
    m_status = tr("Selecione arquivos. Os originais serão preservados.");
    connect(&m_watcher, &QFutureWatcher<Mandarina::FileTools::Outcome>::finished, this, [this] {
        const auto result = m_watcher.result();
        m_outputs = result.outputs; m_errors = result.errors; m_details = result.details;
        if (m_inspecting) { m_metadata = result.metadata; emit metadataChanged(); }
        else if (m_errors.isEmpty() && !result.cancelled) {
            m_changes.clear(); m_remove.clear(); m_removeAll = false; emit draftChanged(); emit metadataChanged();
        }
        m_busy = false;
        m_progress = result.cancelled ? 0 : 1;
        m_status = result.cancelled ? tr("Operação cancelada. Os originais foram preservados.")
            : !m_errors.isEmpty() ? tr("Confira os detalhes abaixo. Os originais foram preservados.")
            : m_inspecting ? tr("Revise os campos. Salvar cria uma nova cópia do arquivo.")
            : tr("Pronto! %1 cópia(s) criada(s). Os originais foram preservados.").arg(m_outputs.size());
        emit changed(); emit busyChanged();
    });
}
FileToolsController::~FileToolsController()
{
    if (m_cancel) m_cancel->store(true);
    m_watcher.waitForFinished();
}
void FileToolsController::setBlocked(bool blocked)
{
    if (m_blocked == blocked) return;
    // This gate belongs to another controller's job. Keep busy() restricted to
    // this controller's own worker, avoiding reciprocal busy dependencies.
    m_blocked = blocked;
    emit changed();
}
void FileToolsController::setOutputDirectory(const QString &directory)
{
    const auto path = Mandarina::normalizedOutputDirectory(directory);
    if (path == m_outputDirectory) return;
    // Running jobs already own their request snapshot; this preference only
    // changes the destination of the next confirmed operation.
    m_outputDirectory = path;
    emit outputDirectoryChanged();
}
QStringList FileToolsController::localFiles(const QVariantList &urls)
{
    QStringList paths;
    for (const auto &value : urls) {
        const QUrl url = value.canConvert<QUrl>() ? value.toUrl() : QUrl(value.toString());
        const QString raw = url.isLocalFile() ? url.toLocalFile() : url.scheme().isEmpty() ? value.toString() : QString();
        if (raw.isEmpty()) continue;
        const QFileInfo file(raw);
        if (!file.isFile() || !file.isReadable()) continue;
        const auto path = file.canonicalFilePath();
        if (!path.isEmpty() && !paths.contains(path)) paths << path;
        if (paths.size() >= 128) break;
    }
    return paths;
}
QVariantList FileToolsController::toolsFormats()
{
    return {QVariantMap{{"id", "tool:compress"}, {"label", "COMPRIMIR"}, {"detail", tr("Reduzir tamanho · ZIP")}},
            QVariantMap{{"id", "tool:metadata"}, {"label", "METADADOS"}, {"detail", tr("Ver, editar ou remover")}},
            QVariantMap{{"id", "tool:photo"}, {"label", "EDITAR FOTO"}, {"detail", tr("Luz, cor e detalhes")}}};
}
QVariantList FileToolsController::toolsFormatsForFiles(const QStringList &paths)
{
    auto formats = toolsFormats();
    if (paths.size() != 1 || !Mandarina::Photo::supportsPath(paths.first())) formats.removeLast();
    return formats;
}
QVariantList FileToolsController::files() const
{
    QVariantList result;
    for (const auto &path : m_files) {
        QFileInfo file(path);
        result << QVariantMap{{"path",path},{"name",file.fileName()},{"bytes",file.size()},
            {"size", file.size() < 1048576 ? tr("%1 KB").arg(file.size()/1024.0,0,'f',1) : tr("%1 MB").arg(file.size()/1048576.0,0,'f',1)}};
    }
    return result;
}
QVariantList FileToolsController::metadata() const
{
    QVariantList rows;
    for (const auto &value : m_metadata) {
        auto row = value.toMap();
        const auto key = row.value("key").toString();
        row["draft"] = m_changes.value(key, row.value("value"));
        row["removed"] = m_remove.contains(key) || (m_removeAll && row.value("removable").toBool());
        rows << row;
    }
    return rows;
}
void FileToolsController::openFiles(const QStringList &paths, const QString &action)
{
    if (m_busy || m_blocked) return;
    QVariantList urls; for (const auto &path : paths) urls << QUrl::fromLocalFile(path);
    if (action == QStringLiteral("photo")) {
        // Photo editing has its own controller and floating window. Opening it
        // must preserve any compression selection or metadata draft here.
        openUrls(urls, action);
        return;
    }
    m_files = localFiles(urls); m_current = -1; m_metadata.clear(); m_changes.clear(); m_remove.clear(); m_removeAll=false;
    m_outputs.clear(); m_errors.clear(); m_details.clear();
    m_action = action == "metadata" ? "metadata" : "compress";
    m_status = tr("Selecione as opções e confirme. Os originais serão preservados.");
    emit filesChanged(); emit metadataChanged(); emit draftChanged(); emit changed(); emit panelRequested();
    if (m_action == "metadata" && !m_files.isEmpty()) inspect(0);
}
void FileToolsController::addUrls(const QVariantList &urls)
{
    if (m_busy || m_blocked) return;
    openFiles(localFiles(urls), m_action);
}
bool FileToolsController::openUrls(const QVariantList &urls, const QString &action)
{
    if (m_busy || m_blocked || (action != "compress" && action != "metadata" && action != "photo")) return false;
    if (action == QStringLiteral("photo") && urls.size() != 1) return false;
    QStringList paths;
    // Select the complete incoming set before replacing the active tool state.
    // Invalid entries must not quietly open a panel for only a valid subset.
    for (const auto &url : urls) {
        const auto selected = localFiles({url});
        if (selected.size() != 1) return false;
        if (!paths.contains(selected.first())) paths << selected.first();
        if (paths.size() > 128) return false;
    }
    if (action == QStringLiteral("photo")) {
        if (paths.size() != 1 || !Mandarina::Photo::supportsPath(paths.first())) return false;
        emit photoRequested(paths.first());
        return true;
    }
    // Opening compression only presents options. Metadata starts its existing
    // read-only inspection; neither operation writes until explicit confirm.
    openFiles(paths, action);
    return true;
}
void FileToolsController::setAction(const QString &action)
{
    if (m_busy || m_blocked || (action != "compress" && action != "metadata") || m_action == action) return;
    m_action = action; m_outputs.clear(); m_errors.clear(); m_details.clear(); emit changed();
    if (action == "metadata" && !m_files.isEmpty()) inspect(qMax(0,m_current));
}
void FileToolsController::inspect(int index)
{
    if (m_busy || m_blocked || index < 0 || index >= m_files.size()) return;
    m_current = index; m_metadata.clear(); m_changes.clear(); m_remove.clear(); m_removeAll=false;
    emit metadataChanged(); emit draftChanged();
    Mandarina::FileTools::Request request; request.paths = {m_files[index]};
    launch(request, true);
}
void FileToolsController::editMetadata(const QString &key, const QString &value)
{
    if (m_busy || m_blocked || value.size() > 4096) return;
    for (const auto &row : m_metadata) {
        const auto entry = row.toMap();
        if (entry.value("key").toString() == key && entry.value("editable").toBool()) {
            if (entry.value("value").toString() == value) m_changes.remove(key);
            else m_changes[key] = value;
            emit draftChanged(); return;
        }
    }
}
void FileToolsController::removeMetadata(const QString &key, bool remove)
{
    if (m_busy || m_blocked) return;
    for (const auto &row : m_metadata) {
        const auto entry = row.toMap();
        if (entry.value("key").toString() == key && entry.value("removable").toBool()) {
            m_remove.removeAll(key); if (remove) m_remove << key;
            emit metadataChanged(); emit draftChanged(); return;
        }
    }
}
void FileToolsController::removeAllMetadata(bool remove)
{
    if (m_busy || m_blocked) return;
    m_removeAll = remove; emit metadataChanged(); emit draftChanged();
}
void FileToolsController::saveMetadata()
{
    if (m_busy || m_blocked || m_current < 0 || !metadataDirty()) return;
    Mandarina::FileTools::Request request;
    request.paths = {m_files[m_current]}; request.action = "metadata";
    request.outputDirectory = m_outputDirectory;
    auto changes=m_removeAll ? QVariantMap{} : m_changes;
    for (const auto &key : m_remove) changes.remove(key);
    request.options = {{"changes",changes},{"remove",m_remove},{"removeAll",m_removeAll}};
    launch(request);
}
void FileToolsController::compress(const QString &preset, const QString &mode, int maxEdge, double targetPercent, qint64 targetBytes)
{
    if (m_busy || m_blocked || m_files.isEmpty()) return;
    Mandarina::FileTools::Request request;
    request.paths=m_files; request.action="compress";
    request.outputDirectory = m_outputDirectory;
    request.options={{"preset",preset},{"mode",mode},{"maxEdge",maxEdge},{"targetPercent",targetPercent},{"targetBytes",targetBytes}};
    launch(request);
}
void FileToolsController::launch(const Mandarina::FileTools::Request &request, bool inspecting)
{
    if (m_busy || m_blocked) return;
    m_busy=true; m_inspecting=inspecting; m_progress=0; m_outputs.clear(); m_errors.clear(); m_details.clear();
    m_status = inspecting ? tr("Lendo metadados localmente…") : tr("Processando uma nova cópia…");
    m_cancel=std::make_shared<std::atomic_bool>(false);
    emit changed(); emit busyChanged();
    const QPointer<FileToolsController> self(this);
    const auto cancel=m_cancel;
    m_watcher.setFuture(QtConcurrent::run([self,request,cancel,inspecting] {
        if (inspecting) return Mandarina::FileTools::inspect(request.paths.first(),*cancel);
        return Mandarina::FileTools::run(request,*cancel,[self](int done,int total,const QString &message) {
            if (!self) return;
            QMetaObject::invokeMethod(self,[self,done,total,message] {
                if (!self || !self->m_busy) return;
                self->m_progress = total > 0 ? double(done)/total : 0;
                self->m_status=message; emit self->changed();
            },Qt::QueuedConnection);
        });
    }));
}
void FileToolsController::cancel() { if(m_cancel) m_cancel->store(true); }
void FileToolsController::openOutputFolder()
{
    const auto path = !m_outputs.isEmpty() ? m_outputs.first() : m_files.value(0);
    if (!path.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
}
