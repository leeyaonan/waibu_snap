// 独立外部测量进程，不链接进应用，不改变应用空闲策略。
#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <libproc.h>
#include <mach/mach_time.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <vector>
namespace
{
QString sysctlText(const char* name)
{
    size_t length = 0;
    if (sysctlbyname(name, nullptr, &length, nullptr, 0) != 0)
        return {};
    QByteArray buffer(int(length), '\0');
    if (sysctlbyname(name, buffer.data(), &length, nullptr, 0) != 0)
        return {};
    return QString::fromUtf8(buffer.constData());
}
quint64 sysctlNumber(const char* name)
{
    quint64 value = 0;
    size_t length = sizeof(value);
    if (sysctlbyname(name, &value, &length, nullptr, 0) != 0)
        return 0;
    return value;
}
quint64 cpuTimeNs(const rusage_info_v2& usage)
{
    static const mach_timebase_info_data_t base = []
    {
        mach_timebase_info_data_t result = {};
        mach_timebase_info(&result);
        return result;
    }();
    // XNU CPU 时间使用 Mach absolute units，按本机 timebase 转为纳秒。
    const quint64 ticks = usage.ri_user_time + usage.ri_system_time;
    return (ticks / base.denom) * base.numer + (ticks % base.denom) * base.numer / base.denom;
}
QJsonObject calibrateCpu()
{
    rusage_info_v2 before = {}, after = {};
    rusage referenceBefore = {}, referenceAfter = {};
    if (proc_pid_rusage(getpid(), RUSAGE_INFO_V2, reinterpret_cast<rusage_info_t*>(&before)) != 0 ||
        getrusage(RUSAGE_SELF, &referenceBefore) != 0)
        return {{"valid", false}};
    volatile quint64 work = 0;
    const auto finish = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (std::chrono::steady_clock::now() < finish)
        work += 1;
    getrusage(RUSAGE_SELF, &referenceAfter);
    proc_pid_rusage(getpid(), RUSAGE_INFO_V2, reinterpret_cast<rusage_info_t*>(&after));
    const auto referenceNs = [](const rusage& value)
    {
        return (quint64(value.ru_utime.tv_sec) + quint64(value.ru_stime.tv_sec)) * 1000000000ULL +
               (quint64(value.ru_utime.tv_usec) + quint64(value.ru_stime.tv_usec)) * 1000ULL;
    };
    const auto delta = cpuTimeNs(after) - cpuTimeNs(before);
    const auto reference = referenceNs(referenceAfter) - referenceNs(referenceBefore);
    const double ratio = reference ? double(delta) / double(reference) : 0;
    return {{"valid", ratio > 0.9 && ratio < 1.1},
            {"libproc_cpu_ns", QString::number(delta)},
            {"getrusage_cpu_ns", QString::number(reference)},
            {"ratio", ratio}};
}
QJsonObject metadata()
{
    [NSApplication sharedApplication];
    QJsonArray displays;
    for (NSScreen* screen in NSScreen.screens)
    {
        const auto id = [screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue];
        CGDisplayModeRef mode = CGDisplayCopyDisplayMode(id);
        const NSRect frame = screen.frame;
        displays.append(QJsonObject{
            {"id", int(id)},
            {"logical_x", frame.origin.x},
            {"logical_y", frame.origin.y},
            {"logical_width", frame.size.width},
            {"logical_height", frame.size.height},
            {"backing_scale", screen.backingScaleFactor},
            {"mode_pixel_width", double(mode ? CGDisplayModeGetPixelWidth(mode) : 0)},
            {"mode_pixel_height", double(mode ? CGDisplayModeGetPixelHeight(mode) : 0)},
            {"mode_refresh_hz", mode ? CGDisplayModeGetRefreshRate(mode) : 0},
            {"maximum_fps", double(screen.maximumFramesPerSecond)},
            {"minimum_refresh_ms", screen.minimumRefreshInterval * 1000},
            {"maximum_refresh_ms", screen.maximumRefreshInterval * 1000},
            {"maximum_edr_component", screen.maximumExtendedDynamicRangeColorComponentValue}});
        if (mode)
            CGDisplayModeRelease(mode);
    }
    TISInputSourceRef source = TISCopyCurrentKeyboardInputSource();
    const auto sourceId =
        source ? (__bridge NSString*)TISGetInputSourceProperty(source, kTISPropertyInputSourceID)
               : nil;
    const QString input =
        sourceId ? QString::fromUtf8(sourceId.UTF8String) : QStringLiteral("不可读取");
    if (source)
        CFRelease(source);
    mach_timebase_info_data_t base = {};
    mach_timebase_info(&base);
    return {{"probe_version", "2"},
            {"cpu_native_unit", "mach_absolute_time"},
            {"timebase_numer", int(base.numer)},
            {"timebase_denom", int(base.denom)},
            {"cpu", sysctlText("machdep.cpu.brand_string")},
            {"model", sysctlText("hw.model")},
            {"memory_bytes", double(sysctlNumber("hw.memsize"))},
            {"logical_cores", double(sysctlNumber("hw.logicalcpu"))},
            {"displays", displays},
            {"input_source_id", input}};
}
QJsonObject sample(int rootPid, int windowServerPid)
{
    std::vector<pid_t> pids(size_t(std::max(proc_listallpids(nullptr, 0) + 256, 4096)));
    const int count = proc_listallpids(pids.data(), int(pids.size() * sizeof(pid_t)));
    struct Entry
    {
        int pid;
        int parent;
        QString name;
    };
    std::vector<Entry> entries;
    for (int index = 0; index < count; ++index)
    {
        proc_bsdinfo info = {};
        if (proc_pidinfo(pids[index], PROC_PIDTBSDINFO, 0, &info, sizeof(info)) == sizeof(info))
            entries.push_back(
                {int(pids[index]), int(info.pbi_ppid), QString::fromUtf8(info.pbi_comm)});
    }
    if (windowServerPid > 1 &&
        std::none_of(entries.begin(), entries.end(), [windowServerPid](const Entry& entry)
                     { return entry.pid == windowServerPid; }))
        entries.push_back({windowServerPid, 0, QStringLiteral("WindowServer")});
    if (std::none_of(entries.begin(), entries.end(),
                     [rootPid](const Entry& entry) { return entry.pid == rootPid; }))
        entries.push_back({rootPid, 0, QStringLiteral("被测进程")});
    std::vector<int> family = {rootPid};
    bool changed = true;
    while (changed)
    {
        changed = false;
        for (const auto& entry : entries)
            if (std::find(family.begin(), family.end(), entry.parent) != family.end() &&
                std::find(family.begin(), family.end(), entry.pid) == family.end())
            {
                family.push_back(entry.pid);
                changed = true;
            }
    }
    QJsonArray app;
    QJsonArray windowServer;
    for (const auto& entry : entries)
    {
        const bool belongs = std::find(family.begin(), family.end(), entry.pid) != family.end();
        if (!belongs && entry.name != QStringLiteral("WindowServer"))
            continue;
        rusage_info_v2 usage = {};
        const int code =
            proc_pid_rusage(entry.pid, RUSAGE_INFO_V2, reinterpret_cast<rusage_info_t*>(&usage));
        QJsonObject value{
            {"pid", entry.pid}, {"ppid", entry.parent}, {"error", code == 0 ? 0 : errno}};
        if (code == 0)
        {
            value.insert("birth", QString::number(usage.ri_proc_start_abstime));
            value.insert("rss_bytes", double(usage.ri_resident_size));
            value.insert("footprint_bytes", double(usage.ri_phys_footprint));
            value.insert("cpu_ns", QString::number(cpuTimeNs(usage)));
        }
        if (belongs)
            app.append(value);
        else
            windowServer.append(value);
    }
    return {{"app", app}, {"window_server", windowServer}};
}
}
int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    const QStringList arguments = application.arguments();
    QJsonObject result;
    if (arguments.size() == 2 && arguments.at(1) == "--self-test")
    {
        result = calibrateCpu();
        QTextStream(stdout) << QJsonDocument(result).toJson(QJsonDocument::Compact) << Qt::endl;
        return result.value("valid").toBool() ? 0 : 3;
    }
    if (arguments.size() == 2 && arguments.at(1) == "--metadata")
        result = metadata();
    else if ((arguments.size() == 3 || arguments.size() == 4) && arguments.at(1) == "--sample")
    {
        bool valid = false;
        const int pid = arguments.at(2).toInt(&valid);
        if (!valid || pid <= 1)
            return 2;
        result = sample(pid, arguments.size() == 4 ? arguments.at(3).toInt() : 0);
    }
    else
        return 2;
    QTextStream(stdout) << QJsonDocument(result).toJson(QJsonDocument::Compact) << Qt::endl;
    return 0;
}
