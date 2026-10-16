// SPDX-License-Identifier: GPL-3.0-only
#include "overlay.hpp"

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        // Keep thread notifications: /MT and the module's thread-local guards
        // need the CRT's attach/detach bookkeeping. Our own detach path is empty.
        // Windows does not run this worker until loader-lock initialization is
        // complete. No DXGI/ImGui/MinHook work and no waits happen in DllMain.
        HANDLE worker = CreateThread(nullptr, 0, framescope::dxgi::InitializeWorker,
                                      module, 0, nullptr);
        if (worker)
            CloseHandle(worker);
        else
            OutputDebugStringW(L"[FrameScope.Hud] Could not start initialization worker.\n");
    }
    // A pinned process-lifetime module: never remove detours during DLL detach.
    return TRUE;
}
