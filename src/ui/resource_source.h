#pragma once
#include <QHash>
#include <QImage>
#include <QObject>
#include <QStringList>
#include <QVector>
namespace waibusnap
{
struct ResourceItem
{
    QString id;
    QString sourceId;
    QString groupId;
    QString fileName;
    QString absolutePath;
    qint64 bytes = 0;
    qint64 originalBytes = 0;
    bool hasOriginal = false;
    bool starred = false;
    bool managed = false;
    qint64 createdAtMs = 0;
    QSize pixelSize;
    // 来源提供展示标签，窗口不判断历史记录的内部类型。
    QStringList badges;
};
struct ResourceGroup
{
    QString id;
    QString name;
    bool isCurrent = false;
    bool isDefault = false;
    QString absolutePath;
};
struct ResourceCapabilities
{
    bool groups = false;
    bool cleanup = false;
    bool originalVariant = false;
    bool star = false;
    bool deleteItem = false;
    bool reveal = false;
};
struct ResourceUsage
{
    qsizetype managedCount = 0;
    qsizetype externalCount = 0;
    qsizetype starredCount = 0;
    qint64 managedBytes = 0;
    qint64 externalBytes = 0;
    qsizetype protectedCount = 0;
    qint64 protectedBytes = 0;
};
struct ResourceSnapshot
{
    QVector<ResourceGroup> groups;
    QVector<ResourceItem> items;
    ResourceUsage usage;
    QHash<QString, ResourceUsage> groupUsage;
    QString error;
    QString notice;
};
struct ResourceStatus
{
    bool success = false;
    QString error;
};
struct ResourceImageResult : ResourceStatus
{
    QImage image;
};
enum class ResourceSort
{
    OldestFirst,
    NewestFirst
};
class ResourceSource : public QObject
{
    Q_OBJECT
  public:
    using QObject::QObject;
    virtual QString id() const = 0;
    virtual QString displayName() const = 0;
    virtual bool visible() const = 0;
    virtual ResourceCapabilities capabilities() const = 0;
    // 快照只读内存；手动刷新由 refresh 显式重扫，不发布 changed。
    virtual ResourceSnapshot snapshot() const = 0;
    virtual void refresh() = 0;
    virtual ResourceImageResult loadImage(const QString& id, QSize bounds = {},
                                          bool original = false) const = 0;
    virtual ResourceStatus setStarred(const QString& id, bool starred) = 0;
    virtual ResourceStatus deleteItem(const QString& id) = 0;
    virtual ResourceStatus revealItem(const QString& id) const = 0;
    virtual void setProtectedItem(const QString& id) = 0;
    virtual bool canCopy() const = 0;
    virtual bool canPin() const = 0;
    virtual ResourceStatus outputImage(const QString& id, bool original, bool pin) const = 0;
  signals:
    void changed();
};
QVector<ResourceItem> sortedResourceItems(QVector<ResourceItem> items, ResourceSort order);
}
