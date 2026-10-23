#pragma once
#include <QString>

namespace Mandarina::RuntimePaths {
// Paths in a portable bundle are relative to runtime-paths.json, never to cwd.
QString root();
QString configured(const QString &key);
QString tool(const QString &name);
QString python();
QString soffice();
QString tessdata();
QString helper(const QString &name);
}
