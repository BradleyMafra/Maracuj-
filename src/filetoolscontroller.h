#pragma once
#include "filetoolsengine.h"
#include <QObject>
#include <QVariantList>
#include <QFutureWatcher>
#include <memory>

class FileToolsController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList files READ files NOTIFY filesChanged)
    Q_PROPERTY(QVariantList metadata READ metadata NOTIFY metadataChanged)
    Q_PROPERTY(QString action READ action NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool blocked READ blocked NOTIFY changed)
    Q_PROPERTY(double progress READ progress NOTIFY changed)
    Q_PROPERTY(QStringList outputs READ outputs NOTIFY changed)
    Q_PROPERTY(QStringList errors READ errors NOTIFY changed)
    Q_PROPERTY(QVariantList details READ details NOTIFY changed)
    Q_PROPERTY(int currentFile READ currentFile NOTIFY metadataChanged)
    Q_PROPERTY(bool metadataDirty READ metadataDirty NOTIFY draftChanged)
    Q_PROPERTY(QString outputDirectory READ outputDirectory WRITE setOutputDirectory NOTIFY outputDirectoryChanged)
    Q_PROPERTY(QVariantList formats READ editFormats CONSTANT)
public:
    explicit FileToolsController(QObject *parent = nullptr);
    ~FileToolsController() override;
    QVariantList files() const;
    QVariantList metadata() const;
    QString action() const { return m_action; }
    QString status() const { return m_status; }
    bool busy() const { return m_busy; }
    bool blocked() const { return m_blocked; }
    void setBlocked(bool blocked);
    double progress() const { return m_progress; }
    QStringList outputs() const { return m_outputs; }
    QStringList errors() const { return m_errors; }
    QVariantList details() const { return m_details; }
    int currentFile() const { return m_current; }
    bool metadataDirty() const { return !m_changes.isEmpty() || !m_remove.isEmpty() || m_removeAll; }
    QString outputDirectory() const { return m_outputDirectory; }
    void setOutputDirectory(const QString &directory);
    void openFiles(const QStringList &paths, const QString &action);
    static QStringList localFiles(const QVariantList &urls);
    static QVariantList toolsFormats();
    static QVariantList toolsFormatsForFiles(const QStringList &paths);
    QVariantList editFormats() const { return toolsFormats(); }
    Q_INVOKABLE bool openUrls(const QVariantList &urls, const QString &action);
    Q_INVOKABLE void addUrls(const QVariantList &urls);
    Q_INVOKABLE void setAction(const QString &action);
    Q_INVOKABLE void inspect(int index);
    Q_INVOKABLE void editMetadata(const QString &key, const QString &value);
    Q_INVOKABLE void removeMetadata(const QString &key, bool remove);
    Q_INVOKABLE void removeAllMetadata(bool remove);
    Q_INVOKABLE void saveMetadata();
    Q_INVOKABLE void compress(const QString &preset, const QString &mode, int maxEdge,
                              double targetPercent, qint64 targetBytes);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void openOutputFolder();
signals:
    void changed();
    void busyChanged();
    void filesChanged();
    void metadataChanged();
    void draftChanged();
    void panelRequested();
    void photoRequested(const QString &path);
    void outputDirectoryChanged();
private:
    void launch(const Mandarina::FileTools::Request &request, bool inspecting = false);
    QStringList m_files, m_outputs, m_errors, m_remove;
    QVariantList m_metadata, m_details;
    QVariantMap m_changes;
    QString m_action = "compress", m_status, m_outputDirectory;
    bool m_busy = false, m_blocked = false, m_inspecting = false, m_removeAll = false;
    int m_current = -1;
    double m_progress = 0;
    std::shared_ptr<std::atomic_bool> m_cancel;
    QFutureWatcher<Mandarina::FileTools::Outcome> m_watcher;
};
