#include "runtimepaths.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

namespace Mandarina::RuntimePaths {
namespace {
QString settingsFile()
{
    const QString override = qEnvironmentVariable("MARACUJA_RUNTIME_CONFIG");
    return override.isEmpty() ? QDir(QCoreApplication::applicationDirPath()).filePath("runtime-paths.json")
                              : QFileInfo(override).absoluteFilePath();
}
QString executable(const QString &relative, const QString &system)
{
    const QString path = QDir(root()).filePath(relative);
    if (QFileInfo(path).isFile()) return path;
#ifdef Q_OS_WIN
    Q_UNUSED(system);
    return path; // Windows packages carry their own tools; do not run arbitrary PATH programs.
#else
    return QStandardPaths::findExecutable(system);
#endif
}
}
QString root() { return QFileInfo(settingsFile()).absolutePath(); }
QString configured(const QString &key)
{
    QFile file(settingsFile());
    if (!file.open(QIODevice::ReadOnly) || file.size() > 64 * 1024) return {};
    const auto object = QJsonDocument::fromJson(file.readAll()).object();
    QString value = object.value(key).toString();
    if (value.isEmpty()) value = object.value("tools").toObject().value(key).toString();
    if (value.isEmpty()) return {};
    return QDir::isAbsolutePath(value) ? QDir::cleanPath(value) : QDir(root()).absoluteFilePath(value);
}
QString tool(const QString &name)
{
    const auto value = configured(name);
    if (!value.isEmpty()) return value;
#ifdef Q_OS_WIN
    return executable("tools/" + name + ".exe", {});
#else
    return executable("tools/" + name, name);
#endif
}
QString python()
{
    const auto value = configured("python");
    if (!value.isEmpty()) return value;
#ifdef Q_OS_WIN
    return executable("runtime/python/python.exe", {});
#else
    return executable("runtime/python/bin/python3", "python3");
#endif
}
QString soffice()
{
    const auto value = configured("soffice");
    if (!value.isEmpty()) return value;
#ifdef Q_OS_WIN
    return executable("runtime/libreoffice/program/soffice.com", {});
#else
    return executable("runtime/libreoffice/program/soffice", "soffice");
#endif
}
QString helper(const QString &name)
{
    const QString bundled = QDir(root()).filePath(name);
    if (QFileInfo(bundled).isFile()) return bundled;
    return QDir(QCoreApplication::applicationDirPath()).filePath("../helpers/" + name);
}
QString tessdata()
{
    const auto value = configured("tessdata");
    if (!value.isEmpty()) return value;
    const QString bundled = QDir(root()).filePath("ocr/tessdata");
    if (QFileInfo(QDir(bundled).filePath("por.traineddata")).isFile()) return bundled;
    return QDir(QCoreApplication::applicationDirPath()).filePath("../vendor/tessdata-fast-4.1.0");
}
}
