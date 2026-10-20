#include "core/history_types.h"
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
namespace waibusnap
{
QString normalizedHistoryGroupName(const QString& name)
{
    return name.trimmed().normalized(QString::NormalizationForm_C);
}
QString historyGroupNameError(const QString& name, const QStringList& existingNames)
{
    for (const QChar ch : name)
        if (ch.category() == QChar::Other_Control || QStringLiteral("<>:\"/\\|?*").contains(ch))
            return QStringLiteral("分组名不能包含控制字符、路径分隔符或 Windows 禁用字符。");
    const QString clean = normalizedHistoryGroupName(name);
    if (clean.isEmpty())
        return QStringLiteral("分组名不能为空。");
    if (clean.startsWith(QLatin1Char('.')) || clean.endsWith(QLatin1Char('.')) ||
        clean.endsWith(QLatin1Char(' ')))
        return QStringLiteral("分组名不能是隐藏目录、点目录或以点、空格结尾。");
    static const QRegularExpression reserved(
        QStringLiteral(R"(^(CON|PRN|AUX|NUL|CLOCK\$|COM[1-9¹²³]|LPT[1-9¹²³])(\.|$))"),
        QRegularExpression::CaseInsensitiveOption);
    if (reserved.match(clean).hasMatch())
        return QStringLiteral("分组名不能使用 Windows 保留名称。");
    for (const auto& existing : existingNames)
        if (normalizedHistoryGroupName(existing).toCaseFolded() == clean.toCaseFolded())
            return QStringLiteral("已存在同名文件或分组（不区分大小写）。");
    return {};
}
QString cleanupPolicyError(const CleanupPolicy& policy)
{
    if (policy.enabled && !policy.ageEnabled && !policy.countEnabled && !policy.bytesEnabled)
        return QStringLiteral("自动清理开启时必须至少启用一项规则。");
    if (policy.maxAgeDays <= 0 || policy.maxCount <= 0 || policy.maxBytes <= 0 ||
        policy.maxBytes > 9007199254740991LL)
        return QStringLiteral(
            "清理策略的天数、张数和字节上限必须为正数，字节上限不能超出 JSON 精确整数范围。");
    return {};
}
QJsonObject cleanupPolicyToJson(const CleanupPolicy& policy)
{
    return {{QStringLiteral("enabled"), policy.enabled},
            {QStringLiteral("ageEnabled"), policy.ageEnabled},
            {QStringLiteral("maxAgeDays"), policy.maxAgeDays},
            {QStringLiteral("countEnabled"), policy.countEnabled},
            {QStringLiteral("maxCount"), policy.maxCount},
            {QStringLiteral("bytesEnabled"), policy.bytesEnabled},
            {QStringLiteral("maxBytes"), policy.maxBytes}};
}
bool cleanupPolicyFromJson(const QJsonObject& json, CleanupPolicy& policy)
{
    for (const auto* key : {"enabled", "ageEnabled", "countEnabled", "bytesEnabled"})
        if (!json.value(QLatin1String(key)).isBool())
            return false;
    for (const auto* key : {"maxAgeDays", "maxCount", "maxBytes"})
    {
        const auto value = json.value(QLatin1String(key));
        const double number = value.toDouble();
        const double maximum = QByteArray(key) == "maxBytes" ? 9007199254740991.0 : 2147483647.0;
        if (!value.isDouble() || number < 1 || number > maximum || std::floor(number) != number)
            return false;
    }
    CleanupPolicy parsed{json.value(QStringLiteral("enabled")).toBool(),
                         json.value(QStringLiteral("ageEnabled")).toBool(),
                         json.value(QStringLiteral("maxAgeDays")).toInt(),
                         json.value(QStringLiteral("countEnabled")).toBool(),
                         json.value(QStringLiteral("maxCount")).toInt(),
                         json.value(QStringLiteral("bytesEnabled")).toBool(),
                         json.value(QStringLiteral("maxBytes")).toInteger()};
    if (!cleanupPolicyError(parsed).isEmpty())
        return false;
    policy = parsed;
    return true;
}
QString historyItemId(const HistoryItem& item)
{
    return item.groupId + QLatin1Char('/') + item.fileName;
}
QVector<HistoryItem> sortedHistoryItems(QVector<HistoryItem> items, HistorySort order)
{
    std::sort(items.begin(), items.end(),
              [order](const HistoryItem& a, const HistoryItem& b)
              {
                  const bool less = a.createdAtMs == b.createdAtMs
                                        ? historyItemId(a) < historyItemId(b)
                                        : a.createdAtMs < b.createdAtMs;
                  const bool equal =
                      a.createdAtMs == b.createdAtMs && historyItemId(a) == historyItemId(b);
                  return order == HistorySort::OldestFirst ? less : !less && !equal;
              });
    return items;
}
GroupUsage historyUsage(const QVector<HistoryItem>& items)
{
    GroupUsage usage;
    for (const auto& item : items)
    {
        if (item.managed)
        {
            ++usage.managedCount;
            usage.managedBytes += item.bytes;
        }
        else
        {
            ++usage.externalCount;
            usage.externalBytes += item.bytes;
        }
        if (item.starred)
            ++usage.starredCount;
    }
    return usage;
}
}
