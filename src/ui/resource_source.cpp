#include "ui/resource_source.h"
#include <algorithm>
namespace waibusnap
{
QVector<ResourceItem> sortedResourceItems(QVector<ResourceItem> items, ResourceSort order)
{
    std::sort(items.begin(), items.end(),
              [order](const ResourceItem& a, const ResourceItem& b)
              {
                  const bool less =
                      a.createdAtMs == b.createdAtMs ? a.id < b.id : a.createdAtMs < b.createdAtMs;
                  const bool equal = a.createdAtMs == b.createdAtMs && a.id == b.id;
                  return order == ResourceSort::OldestFirst ? less : !less && !equal;
              });
    return items;
}
}
