#include "interfaces/autostart.h"
#include <QCoreApplication>
#include <QDir>
#include <qt_windows.h>
namespace waibusnap
{
namespace
{
constexpr wchar_t runKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t valueName[] = L"WaibuSnap";
struct RegistryKey
{
    HKEY handle = nullptr;
    ~RegistryKey()
    {
        if (handle)
            RegCloseKey(handle);
    }
};
QString registryError(const QString& action, LSTATUS code)
{
    wchar_t description[1024] = {};
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                        nullptr, code, 0, description, 1024, nullptr);
    return QStringLiteral("%1失败：%2（系统错误码 %3）")
        .arg(action, length ? QString::fromWCharArray(description, int(length)).trimmed()
                            : QStringLiteral("系统未提供错误详情"))
        .arg(code);
}
class WindowsAutostart final : public Autostart
{
  public:
    AutostartState query() const override
    {
        RegistryKey key;
        LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, runKey, 0, KEY_QUERY_VALUE, &key.handle);
        if (status == ERROR_FILE_NOT_FOUND)
            return {};
        if (status != ERROR_SUCCESS)
            return {false, false, registryError(QStringLiteral("读取登录启动项"), status)};
        DWORD bytes = 0;
        status = RegQueryValueExW(key.handle, valueName, nullptr, nullptr, nullptr, &bytes);
        if (status == ERROR_FILE_NOT_FOUND)
            return {};
        if (status != ERROR_SUCCESS)
            return {false, false, registryError(QStringLiteral("读取登录启动项"), status)};
        return {true, false, {}};
    }
    QString setEnabled(bool enabled) override
    {
        RegistryKey key;
        if (enabled)
        {
            const QString path = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
            if (path.isEmpty())
                return QStringLiteral("无法获取当前应用路径，未注册登录启动项。");
            const auto command = (QLatin1Char('"') + path + QLatin1Char('"')).toStdWString();
            if (command.size() > 260)
                return QStringLiteral(
                    "当前应用路径超过系统登录启动项的 260 字符限制，请移动后重试。");
            LSTATUS status =
                RegCreateKeyExW(HKEY_CURRENT_USER, runKey, 0, nullptr, REG_OPTION_NON_VOLATILE,
                                KEY_SET_VALUE, nullptr, &key.handle, nullptr);
            if (status == ERROR_SUCCESS)
                status = RegSetValueExW(key.handle, valueName, 0, REG_SZ,
                                        reinterpret_cast<const BYTE*>(command.c_str()),
                                        DWORD((command.size() + 1) * sizeof(wchar_t)));
            return status == ERROR_SUCCESS
                       ? QString()
                       : registryError(QStringLiteral("开启登录时自动启动"), status);
        }
        LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, runKey, 0, KEY_SET_VALUE, &key.handle);
        if (status == ERROR_FILE_NOT_FOUND)
            return {};
        if (status == ERROR_SUCCESS)
            status = RegDeleteValueW(key.handle, valueName);
        return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND
                   ? QString()
                   : registryError(QStringLiteral("关闭登录时自动启动"), status);
    }
};
}
std::unique_ptr<Autostart> createAutostart() { return std::make_unique<WindowsAutostart>(); }
}
