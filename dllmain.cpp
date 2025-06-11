// dllmain.cpp : Defines the entry point for the DLL application.
#include "pch.h"

#include <Windows.h>

#include <vector>
#include <string>
#include <cstdlib>

#include "MinHook.h"
#include "utf8_to_utf16.h"

typedef BOOL(WINAPI* ENUMDISPLAYDEVICESA)(LPCSTR, DWORD, PDISPLAY_DEVICEA, DWORD);
ENUMDISPLAYDEVICESA p_EnumDisplayDevicesA = NULL;
ENUMDISPLAYDEVICESA p_original_EnumDisplayDevicesA = NULL;
typedef BOOL(WINAPI* ENUMDISPLAYSETTINGSA)(LPCSTR, DWORD, DEVMODEA*);
ENUMDISPLAYSETTINGSA p_EnumDisplaySettingsA = NULL;
ENUMDISPLAYSETTINGSA p_original_EnumDisplaySettingsA = NULL;

static std::vector<DEVMODEA*> modePtrs{};
static double primaryAspect = -1.0;

BOOL WINAPI detoured_EnumDisplayDevicesA(LPCSTR lpDevice, DWORD iDevNum, PDISPLAY_DEVICEA lpDisplayDevice, DWORD dwFlags) {
    if (lpDevice != NULL || dwFlags != 0) {
        return p_original_EnumDisplayDevicesA(lpDevice, iDevNum, lpDisplayDevice, dwFlags);
    }

    if (iDevNum > 0) {
        return false;
    }

    DWORD devCB = lpDisplayDevice->cb;

    DWORD devIndex = 0;
    ZeroMemory(lpDisplayDevice, devCB);
    lpDisplayDevice->cb = devCB;
    BOOL queryDevResult = p_original_EnumDisplayDevicesA(lpDevice, devIndex, lpDisplayDevice, dwFlags);
    while (queryDevResult) {
        if ((lpDisplayDevice->StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != 0 && (lpDisplayDevice->StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0) {
            break;
        }

        devIndex++;
        ZeroMemory(lpDisplayDevice, devCB);
        lpDisplayDevice->cb = devCB;
        queryDevResult = p_original_EnumDisplayDevicesA(lpDevice, devIndex, lpDisplayDevice, dwFlags);
    }

    if (queryDevResult) {
        while (modePtrs.size() > 0) {
            std::free(modePtrs.back());
            modePtrs.pop_back();
        }

        
        DEVMODEA mode = {};
        DWORD modeIndex = 0;
        ZeroMemory(&mode, sizeof DEVMODEA);
        mode.dmSize = sizeof DEVMODEA;
        mode.dmDriverExtra = 0;
        BOOL queryModeResult = p_original_EnumDisplaySettingsA(lpDisplayDevice->DeviceName, modeIndex, &mode);
        while (queryModeResult) {
            if (mode.dmDriverExtra != 0) {
                MessageBoxW(NULL, utf8_to_utf16(u8"Somehow EnumDisplaySettingsA returns driver data that we didn't ask. Memory is corrupted.").data(), utf8_to_utf16(u8"no1600x1200").data(), MB_OK | MB_ICONINFORMATION | MB_SYSTEMMODAL);
                mode.dmDriverExtra = 0;
            }

            double modeAspect = static_cast<double>(mode.dmPelsWidth) / static_cast<double>(mode.dmPelsHeight);
            if (std::abs(modeAspect - primaryAspect) < 0.1) {
                // NTSC display works at 59.9, which would be truncated to 59 by C data type.
                // This is a dirty hack from Linux WINE 10.1 https://gitlab.winehq.org/wine/wine/-/merge_requests/7277
                // It should be fine because DirectX 9 would default to the closest supported refresh rate below those unsupported refresh rate. https://learn.microsoft.com/en-us/windows/win32/api/d3d9helper/nf-d3d9helper-idirect3d9-createdevice
                if (mode.dmDisplayFrequency == 143) {
                    mode.dmDisplayFrequency = 144;
                }
                if (mode.dmDisplayFrequency != 144) {
                    if (mode.dmDisplayFrequency % 5 == 4) {
                        mode.dmDisplayFrequency++;
                    }
                }

                auto modeStorage = reinterpret_cast<DEVMODEA*>(std::calloc(1, sizeof DEVMODEA));
                if (modeStorage == NULL) {
                    MessageBoxW(NULL, utf8_to_utf16(u8"Failed to allocate memory for display mode struct.").data(), utf8_to_utf16(u8"no1600x1200").data(), MB_OK | MB_ICONINFORMATION | MB_SYSTEMMODAL);
                    break;
                }

                std::memcpy(modeStorage, &mode, sizeof DEVMODEA);
                modePtrs.push_back(modeStorage);
            }
            modeIndex++;
            ZeroMemory(&mode, sizeof DEVMODEA);
            mode.dmSize = sizeof DEVMODEA;
            mode.dmDriverExtra = 0;
            queryModeResult = p_original_EnumDisplaySettingsA(lpDisplayDevice->DeviceName, modeIndex, &mode);
        }
    }

    return queryDevResult;
}

BOOL WINAPI detoured_EnumDisplaySettingsA(LPCSTR lpszDeviceName, DWORD iModeNum, DEVMODEA* lpDevMode) {
    if (lpszDeviceName == NULL) {
        return p_original_EnumDisplaySettingsA(lpszDeviceName, iModeNum, lpDevMode);
    }

    if (iModeNum == ENUM_CURRENT_SETTINGS || iModeNum == ENUM_REGISTRY_SETTINGS) {
        return p_original_EnumDisplaySettingsA(lpszDeviceName, iModeNum, lpDevMode);
    }

    if (iModeNum >= modePtrs.size()) {
        return false;
    }

    if (lpDevMode->dmSize < modePtrs[iModeNum]->dmSize) {
        MessageBoxW(NULL, utf8_to_utf16(u8"Game is providing less memory than needed for EnumDisplaySettingsA mode data.").data(), utf8_to_utf16(u8"no1600x1200").data(), MB_OK | MB_ICONINFORMATION | MB_SYSTEMMODAL);
        return false;
    }

    std::memcpy(lpDevMode, modePtrs[iModeNum], modePtrs[iModeNum]->dmSize);
    return true;
}

BOOL APIENTRY DllMain(HMODULE hModule,
    DWORD  ul_reason_for_call,
    LPVOID lpReserved
)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
    {
        HMODULE testVMM = GetModuleHandleA(u8"VanillaMultiMonitorFix.dll");
        if (testVMM != NULL) {
            MessageBoxW(NULL, utf8_to_utf16(u8"no1600x1200 can not work alongside VanillaMultiMonitorFix. Please make a choice.").data(), utf8_to_utf16(u8"no1600x1200").data(), MB_OK | MB_ICONINFORMATION | MB_SYSTEMMODAL);
            return FALSE;
        }

        primaryAspect = static_cast<double>(GetSystemMetrics(SM_CXSCREEN)) / static_cast<double>(GetSystemMetrics(SM_CYSCREEN));

        if (MH_Initialize() != MH_OK) {
            MessageBoxW(NULL, utf8_to_utf16(u8"Failed to initialize MinHook library.").data(), utf8_to_utf16(u8"no1600x1200").data(), MB_OK | MB_ICONINFORMATION | MB_SYSTEMMODAL);
            return FALSE;
        }
        if (MH_CreateHookApiEx(L"USER32.DLL", "EnumDisplayDevicesA", &detoured_EnumDisplayDevicesA, reinterpret_cast<LPVOID*>(&p_original_EnumDisplayDevicesA), reinterpret_cast<LPVOID*>(&p_EnumDisplayDevicesA)) != MH_OK) {
            MessageBoxW(NULL, utf8_to_utf16(u8"Failed to create hook for EnumDisplayDevicesA function.").data(), utf8_to_utf16(u8"no1600x1200").data(), MB_OK | MB_ICONINFORMATION | MB_SYSTEMMODAL);
            return FALSE;
        }
        if (MH_CreateHookApiEx(L"USER32.DLL", "EnumDisplaySettingsA", &detoured_EnumDisplaySettingsA, reinterpret_cast<LPVOID*>(&p_original_EnumDisplaySettingsA), reinterpret_cast<LPVOID*>(&p_EnumDisplaySettingsA)) != MH_OK) {
            MessageBoxW(NULL, utf8_to_utf16(u8"Failed to create hook for EnumDisplaySettingsA function.").data(), utf8_to_utf16(u8"no1600x1200").data(), MB_OK | MB_ICONINFORMATION | MB_SYSTEMMODAL);
            return FALSE;
        }
        if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
            MessageBoxW(NULL, utf8_to_utf16(u8"Failed to disable hooks during DLL_PROCESS_ATTACH.").data(), utf8_to_utf16(u8"no1600x1200").data(), MB_OK | MB_ICONINFORMATION | MB_SYSTEMMODAL);
            return EXIT_FAILURE;
        }
        break;
    }
    case DLL_THREAD_ATTACH:
    {
        break;
    }
    case DLL_THREAD_DETACH:
    {
        break;
    }
    case DLL_PROCESS_DETACH:
    {
        if (lpReserved == NULL) {
            if (MH_DisableHook(MH_ALL_HOOKS) != MH_OK) {
                MessageBoxW(NULL, utf8_to_utf16(u8"Failed to disable hooks. Game might crash later.").data(), utf8_to_utf16(u8"no1600x1200").data(), MB_OK | MB_ICONINFORMATION | MB_SYSTEMMODAL);
                return FALSE;
            }
            if (MH_RemoveHook(p_EnumDisplayDevicesA) != MH_OK) {
                MessageBoxW(NULL, utf8_to_utf16(u8"Failed to remove hook for EnumDisplayDevicesA function. Game might crash later.").data(), utf8_to_utf16(u8"no1600x1200").data(), MB_OK | MB_ICONINFORMATION | MB_SYSTEMMODAL);
                return FALSE;
            }
            if (MH_RemoveHook(p_EnumDisplaySettingsA) != MH_OK) {
                MessageBoxW(NULL, utf8_to_utf16(u8"Failed to remove hook for EnumDisplaySettingsA function. Game might crash later.").data(), utf8_to_utf16(u8"no1600x1200").data(), MB_OK | MB_ICONINFORMATION | MB_SYSTEMMODAL);
                return FALSE;
            }
            if (MH_Uninitialize() != MH_OK) {
                MessageBoxW(NULL, utf8_to_utf16(u8"Failed to uninitialize MinHook. Game might crash later.").data(), utf8_to_utf16(u8"no1600x1200").data(), MB_OK | MB_ICONINFORMATION | MB_SYSTEMMODAL);
                return FALSE;
            }
            while (modePtrs.size() > 0) {
                std::free(modePtrs.back());
                modePtrs.pop_back();
            }
        }
        break;
    }
    }
    return TRUE;
}
