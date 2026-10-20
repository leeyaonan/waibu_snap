#pragma once
#include <QString>
#include <memory>
namespace waibusnap
{
struct FileManagerResult
{
    bool success = false;
    QString error;
};
class FileManager
{
  public:
    virtual ~FileManager() = default;
    virtual FileManagerResult revealInFolder(const QString& path) = 0;
    virtual FileManagerResult openFolder(const QString& path) = 0;
    // 失败必须保留原文件，绝不降级为永久删除。
    virtual FileManagerResult moveToTrash(const QString& path) = 0;
};
std::unique_ptr<FileManager> createFileManager();
}
