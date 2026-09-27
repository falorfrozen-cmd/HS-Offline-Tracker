// Stands in for AurieCore.dll in exit_teardown_smoke.cpp. When the Aurie
// framework is unloaded (the Aurie console's "Unload framework"), AurieCore's
// DLL_PROCESS_DETACH calls every module's ModuleUnload and then FreeLibrary on
// it (ArProcessDetach -> MdpUnmapImage), all with the loader lock held. This
// module does the same to exit_teardown_probe.dll.

#include <Windows.h>

namespace {

using ProbeStartFunction = int (*)(int) noexcept;
using ProbeUnloadFunction = int (*)() noexcept;

HMODULE g_probe{};
ProbeUnloadFunction g_probe_unload{};

} // namespace

// Loads the probe and starts it in its current shape. Returns 0 on success.
extern "C" __declspec(dllexport) int HostStart(const wchar_t* probe_path) noexcept {
    g_probe = LoadLibraryW(probe_path);
    if (!g_probe) {
        return 1;
    }
    const auto start = reinterpret_cast<ProbeStartFunction>(GetProcAddress(g_probe, "ProbeStart"));
    g_probe_unload = reinterpret_cast<ProbeUnloadFunction>(GetProcAddress(g_probe, "ProbeUnload"));
    if (!start || !g_probe_unload) {
        return 2;
    }
    return start(0) == 0 ? 0 : 3;
}

BOOL WINAPI DllMain(HINSTANCE, const DWORD reason, LPVOID reserved) {
    // A FreeLibrary, not the process ending: AurieCore does nothing then.
    if (reason == DLL_PROCESS_DETACH && reserved == nullptr && g_probe) {
        static_cast<void>(g_probe_unload());
        FreeLibrary(g_probe);
    }
    return TRUE;
}
