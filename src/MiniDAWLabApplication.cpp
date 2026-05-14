#include "MiniDAWLabApplication.h"

#include "app/MainAppWindow.h"
#include "audio/LatencySettingsStore.h"
#include "domain/Session.h"
#include "engine/CountInClickOutput.h"
#include "engine/PlaybackEngine.h"
#include "engine/RecorderService.h"
#include "plugins/PluginInsertHost.h"
#include "transport/Transport.h"

MiniDAWLabApplication::~MiniDAWLabApplication() = default;
