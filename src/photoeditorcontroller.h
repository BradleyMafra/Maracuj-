#pragma once
#include "photoengine.h"
#include <QObject>
#include <QFutureWatcher>
#include <QQuickImageProvider>
#include <QTimer>
#include <QUrl>
#include <memory>

struct PhotoPreviewStore;
struct PhotoEditorWork {
    enum Kind { Load, Preview, Export } kind = Load;
    quint64 generation = 0;
    QImage image;
    QString output, error;
    QVariantMap adjustments;
    bool cancelled = false;
};

class PhotoEditorController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString fileName READ fileName NOTIFY changed)
    Q_PROPERTY(QString inputPath READ inputPath NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QString outputPath READ outputPath NOTIFY changed)
    Q_PROPERTY(QUrl previewSource READ previewSource NOTIFY previewChanged)
    Q_PROPERTY(QUrl originalSource READ originalSource NOTIFY previewChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool cancellable READ cancellable NOTIFY busyChanged)
    Q_PROPERTY(bool rendering READ rendering NOTIFY changed)
    Q_PROPERTY(bool ready READ ready NOTIFY changed)
    Q_PROPERTY(bool dirty READ dirty NOTIFY changed)
    Q_PROPERTY(QVariantMap adjustments READ adjustments NOTIFY changed)
    Q_PROPERTY(QVariantList controls READ controls CONSTANT)
    Q_PROPERTY(QStringList exportFormats READ exportFormats CONSTANT)
    Q_PROPERTY(QString exportFormat READ exportFormat WRITE setExportFormat NOTIFY changed)
    Q_PROPERTY(int quality READ quality WRITE setQuality NOTIFY changed)
public:
    explicit PhotoEditorController(QObject *parent = nullptr);
    ~PhotoEditorController() override;
    QQuickImageProvider *createImageProvider() const;
    QString fileName() const;
    QString inputPath() const { return m_input; }
    QString status() const { return m_status; }
    QString error() const { return m_error; }
    QString outputPath() const { return m_output; }
    QUrl previewSource() const;
    QUrl originalSource() const;
    bool busy() const { return m_loading || m_exporting || m_externalBusy; }
    bool cancellable() const { return m_loading || m_exporting; }
    bool rendering() const { return m_previewWanted || (m_running && m_kind == PhotoEditorWork::Preview); }
    bool ready() const { return !m_original.isNull(); }
    bool dirty() const { return ready() && m_adjustments != m_savedAdjustments; }
    QVariantMap adjustments() const { return m_adjustments; }
    QVariantList controls() const;
    QStringList exportFormats() const { return Mandarina::Photo::outputFormats(); }
    QString exportFormat() const { return m_format; }
    void setExportFormat(const QString &format);
    int quality() const { return m_quality; }
    void setQuality(int quality);
    void setOutputDirectory(const QString &directory);
    void setExternalBusy(bool busy);
    bool openFile(const QString &path);
    Q_INVOKABLE void setAdjustment(const QString &id, double value);
    Q_INVOKABLE void resetAdjustment(const QString &id);
    Q_INVOKABLE void resetAll();
    Q_INVOKABLE void saveCopy();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void openOutputFolder();
    Q_INVOKABLE void closeEditor();
signals:
    void changed();
    void previewChanged();
    void busyChanged();
    void panelRequested();
private:
    void loadPending();
    void startPreview();
    void queuePreview();
    void workFinished();
    void publishPreview(const QImage &image, bool original = false);
    QVariantMap neutralAdjustments() const;
    std::shared_ptr<PhotoPreviewStore> m_store;
    QImage m_original;
    QString m_input, m_pendingPath, m_output, m_error, m_status, m_directory;
    QString m_format = QStringLiteral("png");
    QVariantMap m_adjustments, m_savedAdjustments;
    int m_quality = 92;
    quint64 m_generation = 0, m_revision = 0, m_originalRevision = 0;
    bool m_active = false, m_loading = false, m_exporting = false, m_externalBusy = false;
    bool m_running = false, m_previewWanted = false;
    PhotoEditorWork::Kind m_kind = PhotoEditorWork::Load;
    QTimer m_debounce;
    std::shared_ptr<std::atomic_bool> m_cancel;
    QFutureWatcher<PhotoEditorWork> m_watcher;
};
