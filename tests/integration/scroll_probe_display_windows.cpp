#include "interfaces/display_topology.h"
waibusnap::DisplayResult scrollProbeDisplay(int)
{
    return {{}, QStringLiteral("Windows 区域采集后补。")};
}

bool scrollProbeActivate(const QString&) { return false; }
