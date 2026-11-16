#include "exec_filter.h"
#include <cstring>
#include "replay_bindings.h"

namespace execfilter {
namespace {
using ExecFromFastfile = bool (*)(int, int, const char*, bool);
std::atomic<ExecFromFastfile> g_exec{nullptr};

bool Exec(int localClient, int controller, const char* name, bool flag) {
    if (name && std::strncmp(name, "devgui_", 7) == 0)
        return false;
    return g_exec.load(std::memory_order_acquire)(localClient, controller, name, flag);
}
}

hook::Status Install(uintptr_t imageBase) {
    return hook::Install(reinterpret_cast<void*>(imageBase + replay::ExecFromFastfile.rva), &Exec,
                         replay::ExecFromFastfile.bytes, replay::ExecFromFastfile.size, g_exec);
}
}
