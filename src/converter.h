#pragma once
#include "conversionengine.h"
#include <QObject>
#include <QVariantList>
#include <QFutureWatcher>
#include <QThreadPool>
#include <QUrl>
#include <memory>

class Converter final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList files READ files NOTIFY filesChanged)
    Q_PROPERTY(QVariantList formats READ formats NOTIFY formatsChanged)
    Q_PROPERTY(bool formatsLoading READ formatsLoading NOTIFY formatsChanged)
    Q_PROPERTY(QString inputKind READ inputKind NOTIFY formatsChanged)
    Q_PROPERTY(QString selectedFormat READ selectedFormat WRITE setSelectedFormat NOTIFY selectedFormatChanged)
    Q_PROPERTY(QString outputDirectory READ outputDirectory WRITE setOutputDirectory NOTIFY outputDirectoryChanged)
    Q_PROPERTY(QUrl outputFolderUrl READ outputFolderUrl NOTIFY outputFolderChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool dragPreview READ dragPreview NOTIFY formatsChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QStringList errors READ errors NOTIFY resultChanged)
    Q_PROPERTY(QStringList outputs READ outputs NOTIFY resultChanged)
    Q_PROPERTY(int quality MEMBER m_quality NOTIFY qualityChanged)
    Q_PROPERTY(int dpi MEMBER m_dpi NOTIFY dpiChanged)
public:
    explicit Converter(QObject *parent = nullptr);
    ~Converter() override;
    QVariantList files() const;
    QVariantList formats() const;
    bool formatsLoading() const;
    QString inputKind() const;
    QString selectedFormat() const { return m_format; }
    void setSelectedFormat(const QString &format);
    QString outputDirectory() const { return m_outputDirectory; }
    void setOutputDirectory(const QString &directory);
    QUrl outputFolderUrl() const;
    bool busy() const { return m_busy; }
    bool dragPreview() const { return !m_preview.isEmpty(); }
    double progress() const { return m_progress; }
    QString status() const { return m_status; }
    QStringList errors() const { return m_errors; }
    QStringList outputs() const { return m_outputs; }
    void setInitialFiles(const QStringList &paths);
    Q_INVOKABLE void addUrls(const QVariantList &urls);
    Q_INVOKABLE void addEditingUrls(const QVariantList &urls);
    Q_INVOKABLE bool canEditUrls(const QVariantList &urls) const;
    Q_INVOKABLE void removeFile(int index);
    Q_INVOKABLE void clearFiles();
    Q_INVOKABLE bool previewUrls(const QVariantList &urls);
    Q_INVOKABLE void clearDragPreview();
    Q_INVOKABLE void dropOnFormat(const QVariantList &urls, const QString &format);
    // Global wheels discover and convert their own inputs without replacing
    // the main panel's selection or its local drag preview.
    QVariantList formatsForUrls(const QVariantList &urls) const;
    void convertUrls(const QVariantList &urls, const QString &format);
    Q_INVOKABLE void convertTo(const QString &format);
    Q_INVOKABLE void start();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void openOutputFolder();
signals:
    void filesChanged();
    void formatsChanged();
    void selectedFormatChanged();
    void outputDirectoryChanged();
    void outputFolderChanged();
    void busyChanged();
    void progressChanged();
    void statusChanged();
    void resultChanged();
    void qualityChanged();
    void dpiChanged();
private:
    QStringList parseUrls(const QVariantList &urls, QStringList *errors = nullptr,
                          bool allowUnsupported = false) const;
    void appendUrls(const QVariantList &urls, bool editing);
    QString kindFor(const QStringList &paths) const;
    QVariantList formatsForPaths(const QStringList &paths) const;
    void refreshFormats();
    void startRequest(const Mandarina::Request &request);
    void setStatus(const QString &status);
    QStringList m_files, m_preview, m_errors, m_outputs;
    QString m_format = QStringLiteral("png"), m_outputDirectory, m_status;
    bool m_busy = false;
    double m_progress = 0;
    int m_quality = 90, m_dpi = 150, m_job = 0;
    std::shared_ptr<std::atomic_bool> m_cancel;
    QFutureWatcher<Mandarina::Result> m_watcher;
#ifdef Q_OS_WIN
    QThreadPool m_formatPool;
    bool m_documentFormatsLoading = false, m_audioFormatsLoading = false;
    QStringList m_audioFormats;
    QFutureWatcher<void> m_documentFormatsWatcher;
    QFutureWatcher<QStringList> m_audioFormatsWatcher;
#endif
};
