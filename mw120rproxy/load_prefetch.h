#pragma once
#include <windows.h>
#include <cstdint>

// Disk read recorder and read-ahead for a game folder on a hard disk.
// The recorder stores every game-folder read of a run in mw120rproxy.prefetch.txt.
// The next run reads the same ranges in disk order before the game needs them.
namespace loadprefetch {
struct Settings {
    bool record = true;
    bool replay = true;
};

// Safe during DLL attach: patches import slots of the game image and starts threads.
void Start(HMODULE self, uintptr_t imageBase, const Settings& settings);
}
