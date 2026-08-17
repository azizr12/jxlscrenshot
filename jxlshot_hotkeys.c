/* jxlshot_hotkeys.c — global keyboard shortcut handling for jxlshot_tray. */
#ifndef JXLSHOT_HOTKEYS_C
#define JXLSHOT_HOTKEYS_C

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>

static HHOOK g_hhkKeyboard = NULL;

/* File-based logger for background tray apps */
static void debug_log(const char* fmt, ...) {
    FILE* f = fopen("jxlshot_debug.log", "a");
    if (f) {
        va_list args;
        va_start(args, fmt);
        vfprintf(f, fmt, args);
        va_end(args);
        fprintf(f, "\n");
        fclose(f);
    }
}

static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && wParam == WM_KEYDOWN)
    {
        KBDLLHOOKSTRUCT *pKB = (KBDLLHOOKSTRUCT *)lParam;
        
        // TEMPORARY: Log ALL keys to verify the hook is alive and receiving events
        debug_log("Key pressed: vkCode=0x%02X", pKB->vkCode);

        if (pKB->vkCode == VK_SNAPSHOT)
        {
            // GetAsyncKeyState is required for reliable modifier checking in hooks
            if (GetAsyncKeyState(VK_CONTROL) & 0x8000)
            {
                debug_log("ACTION: Ctrl+PrintScreen pressed -> triggering region capture");
                start_region_capture();
            }
            else
            {
                debug_log("ACTION: PrintScreen pressed -> triggering full capture");
                execute_full_capture();
            }

            // CRITICAL: Return 1 to block the key from reaching Windows/Snipping Tool
            return 1; 
        }
    }

    return CallNextHookEx(g_hhkKeyboard, nCode, wParam, lParam);
}

static BOOL register_hotkeys(HWND hwnd)
{
    (void)hwnd; // Unused in WH_KEYBOARD_LL

    if (g_hhkKeyboard != NULL) {
        debug_log("register_hotkeys: Hook already installed.");
        return TRUE;
    }

    HINSTANCE hInst = GetModuleHandle(NULL);
    g_hhkKeyboard = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, hInst, 0);

    if (!g_hhkKeyboard)
    {
        debug_log("register_hotkeys: FAILED to install hook, GetLastError=%lu", GetLastError());
        return FALSE;
    }

    debug_log("register_hotkeys: Hook installed successfully.");
    return TRUE;
}

static void unregister_hotkeys(HWND hwnd)
{
    (void)hwnd;

    if (g_hhkKeyboard != NULL)
    {
        BOOL result = UnhookWindowsHookEx(g_hhkKeyboard);
        debug_log("unregister_hotkeys: Unhook result=%d", result);
        g_hhkKeyboard = NULL;
    }
}

/* Stub to prevent compilation errors if still referenced */
static void handle_hotkey_message(WPARAM wp)
{
    (void)wp; 
}

#endif /* JXLSHOT_HOTKEYS_C */
