#pragma once
#include <QString>
#include <QUrl>

namespace Mandarina {
inline QString normalizedOutputDirectory(const QString &directory)
{
    // Empty is a policy, not a cached folder: each result belongs beside its
    // own source unless the user explicitly selects another destination.
    if (directory.trimmed().isEmpty()) return {};
    const QUrl url(directory);
    return url.isLocalFile() ? url.toLocalFile() : directory;
}
}
