#include "interfaces/file_manager.h"
#import <AppKit/AppKit.h>
#include <QFileInfo>
namespace waibusnap
{
namespace
{
NSURL* fileUrl(const QString& path) { return [NSURL fileURLWithPath:path.toNSString()]; }
class MacFileManager final : public FileManager
{
  public:
    FileManagerResult revealInFolder(const QString& path) override
    {
        if (!QFileInfo::exists(path))
            return {false, QStringLiteral("图片已丢失，请刷新历史。")};
        // 此 API 没有完成回调；成功只表示已提交 Finder 定位请求。
        [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[ fileUrl(path) ]];
        return {true, {}};
    }
    FileManagerResult openFolder(const QString& path) override
    {
        if (!QFileInfo(path).isDir())
            return {false, QStringLiteral("文件夹不存在，无法打开。")};
        const bool opened = [[NSWorkspace sharedWorkspace] openURL:fileUrl(path)];
        return {opened, opened ? QString() : QStringLiteral("无法打开文件夹，请稍后重试。")};
    }
    FileManagerResult moveToTrash(const QString& path) override
    {
        NSError* error = nil;
        const bool moved = [[NSFileManager defaultManager] trashItemAtURL:fileUrl(path)
                                                         resultingItemURL:nil
                                                                    error:&error];
        return {moved, moved ? QString()
                             : QStringLiteral("移入回收站失败：%1")
                                   .arg(QString::fromNSString(error.localizedDescription))};
    }
};
}
std::unique_ptr<FileManager> createFileManager() { return std::make_unique<MacFileManager>(); }
}
