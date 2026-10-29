#pragma once
#include <QString>
namespace waibusnap
{
// 解析最近现存父目录的真实路径，再补回缺失层级；不创建目录。
QString canonicalStorageDirectory(QString path);
// 参数为解析后的绝对路径；相同或互相包含均视为重叠。
bool storageDirectoriesOverlap(const QString& first, const QString& second);
bool storageDirectoryContains(const QString& parent, const QString& child);
}
