#include "app/capture_directory.h"
#include "core/storage_directory.h"
#include <QDir>
#include <QFileInfo>
#include <QTemporaryFile>
namespace waibusnap
{
QString validateCaptureDirectory(const QString& directory, const QStringList& reservedDirectories,
                                 QString& cleaned)
{
    if (directory.isEmpty() || directory.contains(QChar::Null) || !QDir::isAbsolutePath(directory))
        return QStringLiteral("请选择快枪手的绝对目录路径。");
    cleaned = QDir::cleanPath(directory);
    const QFileInfo info(cleaned);
    if (info.isSymLink())
        return QStringLiteral("快枪手目录不能是符号链接。");
    if (!info.exists())
        return QStringLiteral("快枪手目录不存在。");
    if (!info.isDir())
        return QStringLiteral("快枪手路径不是目录。");
    const QString canonical = info.canonicalFilePath();
    if (canonical.isEmpty())
        return QStringLiteral("无法解析快枪手目录的真实路径。");
    for (const auto& reserved : reservedDirectories)
    {
        if (reserved.isEmpty())
            continue;
        const QString other = canonicalStorageDirectory(reserved);
        if (other.isEmpty())
            return QStringLiteral("无法解析保留目录，请确认历史根目录和分组目录后重试。");
        if (storageDirectoriesOverlap(canonical, other))
            return QStringLiteral("快枪手目录不能与历史根目录或分组目录相同，也不能互相包含。");
    }
    QTemporaryFile probe(
        QDir(canonical).filePath(QStringLiteral(".waibusnap-quick-gunner-XXXXXX")));
    if (!info.isWritable() || !probe.open() || probe.write("probe", 5) != 5 || !probe.flush())
        return QStringLiteral("快枪手目录不可写，请选择有写入权限的目录。");
    if (!probe.remove())
        return QStringLiteral("快枪手目录写入探针清理失败，请检查目录权限。");
    return {};
}
}
