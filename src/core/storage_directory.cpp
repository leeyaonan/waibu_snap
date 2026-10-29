#include "core/storage_directory.h"
#include <QDir>
#include <QFileInfo>
namespace waibusnap
{
QString canonicalStorageDirectory(QString path)
{
    if (!QDir::isAbsolutePath(path) || path.contains(QChar::Null))
        return {};
    path = QDir::cleanPath(path);
    QStringList missing;
    QFileInfo info(path);
    while (!info.exists())
    {
        if (info.isSymLink() || info.isJunction() || info.fileName().isEmpty())
            return {};
        missing.prepend(info.fileName());
        const auto parent = info.absolutePath();
        if (parent == path)
            return {};
        path = parent;
        info.setFile(path);
    }
    if (!info.isDir())
        return {};
    path = info.canonicalFilePath();
    if (path.isEmpty())
        return {};
    for (const auto& name : missing)
        path = QDir(path).filePath(name);
    return path;
}
bool storageDirectoryContains(const QString& parent, const QString& child)
{
    const auto relative = QDir(parent).relativeFilePath(child);
    return relative == QStringLiteral(".") ||
           (!QDir::isAbsolutePath(relative) && relative != QStringLiteral("..") &&
            !relative.startsWith(QStringLiteral("../")));
}
bool storageDirectoriesOverlap(const QString& first, const QString& second)
{
    return storageDirectoryContains(first, second) || storageDirectoryContains(second, first);
}
}
