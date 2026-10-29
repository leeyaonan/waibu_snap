#pragma once
#include <QString>
int runRecordingControlProbe(const QString& report, const QString& keep, bool retina = false,
                             bool gif = false, const QString& settingsFile = {},
                             bool quitDuringStart = false);
int runRecordingControlTarget(bool retina = false, bool stationary = false, bool large = false,
                              bool fourK = false);
