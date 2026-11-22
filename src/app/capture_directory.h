#pragma once
#include <QString>
#include <QStringList>
namespace waibusnap
{
// 校验截图落盘目录：绝对路径、非符号链接、存在、可写，且不与历史根目录 / 分组目录重叠。
// reservedDirectories 中无法解析的项直接判失败，避免静默跳过保护。
// 空字符串表示通过，cleaned 写入规范化后的绝对路径；否则返回可直接展示的中文原因。
QString validateCaptureDirectory(const QString& directory, const QStringList& reservedDirectories,
                                 QString& cleaned);
}
