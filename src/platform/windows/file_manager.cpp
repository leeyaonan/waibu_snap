#include "interfaces/file_manager.h"
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUrl>
#include <qt_windows.h>
#include <shlobj.h>
namespace waibusnap
{
namespace
{
class WindowsFileManager final : public FileManager
{
  public:
    FileManagerResult revealInFolder(const QString& path) override
    {
        if (!QFileInfo::exists(path))
            return {false, QStringLiteral("图片已丢失，请刷新历史。")};
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
            return {false, QStringLiteral("无法初始化文件管理器（系统错误码 %1）。")
                               .arg(quint32(initialized))};
        const auto native = QDir::toNativeSeparators(path).toStdWString();
        PIDLIST_ABSOLUTE item = nullptr;
        HRESULT result = SHParseDisplayName(native.c_str(), nullptr, &item, 0, nullptr);
        if (SUCCEEDED(result))
            result = SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
        CoTaskMemFree(item);
        if (SUCCEEDED(initialized))
            CoUninitialize();
        return {SUCCEEDED(result),
                SUCCEEDED(result)
                    ? QString()
                    : QStringLiteral("在文件夹中显示失败（系统错误码 %1）。").arg(quint32(result))};
    }
    FileManagerResult openFolder(const QString& path) override
    {
        if (!QFileInfo(path).isDir())
            return {false, QStringLiteral("文件夹不存在，无法打开。")};
        const bool opened = QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        return {opened, opened ? QString() : QStringLiteral("无法打开文件夹，请稍后重试。")};
    }
    FileManagerResult moveToTrash(const QString& path) override
    {
        if (!QFile::supportsMoveToTrash())
            return {false, QStringLiteral("当前系统不支持回收站，原文件已保留。")};
        // Qt 6.11.2 的 PreDeleteItem 会拒绝未设置回收标志的永久删除操作。
        QFile file(path);
        const bool moved = file.moveToTrash();
        return {moved,
                moved ? QString() : QStringLiteral("移入回收站失败：%1").arg(file.errorString())};
    }
};
}
std::unique_ptr<FileManager> createFileManager() { return std::make_unique<WindowsFileManager>(); }
}
