#include <Windows.h>

BOOL WINAPI DllMain(const HINSTANCE instance, const DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        // Deliberately no worker, IPC, fingerprinting, resolver, or hook activity under loader lock.
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

