/* jxlshot_hotkeys.c — global keyboard shortcut handling for jxlshot_tray.
 *
 * INTERCEPTS PrintScreen using a Low-Level Keyboard Hook (WH_KEYBOARD_LL).
 * RegisterHotKey() cannot block the PrintScreen key because Windows reserves 
 * it at the kernel level. This hook intercepts it before the OS processes it.
 *
 * This file is included directly into jxlshot_tray.c.
 */
#ifndef JXLSHOT_HOTKEYS_C
#define JXLSHOT_HOTKEYS_C

static HHOOK g_hhkKeyboard = NULL;

/* The low-level keyboard hook callback. 
 * This runs on the thread that installed the hook (the tray app's UI thread). */
static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && wParam == WM_KEYDOWN)
    {
        KBDLLHOOKSTRUCT *pKB = (KBDLLHOOKSTRUCT *)lParam;

        if (pKB->vkCode == VK_SNAPSHOT)
        {
            // Check if the Ctrl key is currently held down
            if (GetKeyState(VK_CONTROL) & 0x8000)
            {
                dbg("LowLevelHook: Ctrl+PrintScreen pressed -> region capture");
                start_region_capture();
            }
            else
            {
                dbg("LowLevelHook: PrintScreen pressed -> full capture");
                execute_full_capture();
            }

            // CRITICAL: Returning 1 "eats" the keystroke.
            // This prevents Windows from taking its own screenshot or opening the Snipping Tool.
            return 1; 
        }
    }

    // Pass all other keys to the next hook in the chain
    return CallNextHookEx(g_hhkKeyboard, nCode, wParam, lParam);
}

/* Installs the low-level keyboard hook. 
 * The HWND parameter is kept for API compatibility but is unused by WH_KEYBOARD_LL. */
static BOOL register_hotkeys(HWND hwnd)
{
    (void)hwnd; // Suppress unused parameter warning

    if (g_hhkKeyboard != NULL) {
        return TRUE; // Already installed
    }

    // Get the handle to the current module (required for global hooks)
    HINSTANCE hInst = GetModuleHandle(NULL);
    
    // Install the hook globally for the current desktop (dwThreadId = 0)
    g_hhkKeyboard = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, hInst, 0);

    if (!g_hhkKeyboard)
    {
        dbg("register_hotkeys: failed to install keyboard hook, GetLastError=%lu", GetLastError());
        return FALSE;
    }

    dbg("register_hotkeys: low-level keyboard hook installed successfully");
    return TRUE;
}

/* Uninstalls the keyboard hook. Safe to call multiple times. */
static void unregister_hotkeys(HWND hwnd)
{
    (void)hwnd; // Suppress unused parameter warning

    if (g_hhkKeyboard != NULL)
    {
        UnhookWindowsHookEx(g_hhkKeyboard);
        g_hhkKeyboard = NULL;
        dbg("unregister_hotkeys: keyboard hook removed");
    }
}

/* Stub for handle_hotkey_message. 
 * Because we are using a low-level hook instead of RegisterHotKey, 
 * we no longer receive WM_HOTKEY messages in the WndProc. 
 * This function is kept as a no-op to prevent compilation errors 
 * if it is still referenced in jxlshot_tray.c. */
static void handle_hotkey_message(WPARAM wp)
{
    (void)wp; 
}

#endif /* JXLSHOT_HOTKEYS_C */
