#pragma once
#include <cstdint>
#include "inline_hook.h"

// The Replay build execs devgui_*.cfg rawfiles that no fastfile holds. Each exec
// blocks the main thread in DB_FindXAssetHeader until the database thread is idle.
namespace execfilter {
hook::Status Install(uintptr_t imageBase);
}
