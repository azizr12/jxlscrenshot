/* jxlshot_hotkeys.c — global keyboard shortcut handling for jxlshot_tray.
 *
 * Registers two system-wide hotkeys, read from jxlshot.ini via g_cfg
 * (see jxlshot.c / init_config()):
 *
 *   HotkeyFull   (default: PrintScreen)      -> full monitor capture
 *   HotkeyRegion (default: Ctrl+PrintScreen) -> region capture
 *
 * This file is included directly into jxlshot_tray.c, the same way
 * jxlshot_tray.c already includes jxlshot.c, so it shares g_cfg, dbg(),
 * execute_full_capture() and start_region_capture() with the rest of
 * the tray app. It is not a separate process — the hotkeys are handled
 * on the tray app's own message loop via WM_HOTKEY.
 */
#ifndef JXLSHOT_HOTKEYS_C
#define JXLSHOT_HOTKEYS_C

#define HOTKEY_ID_FULL   1
#define HOTKEY_ID_REGION 2

/* Registers the full-capture and region-capture hotkeys against hwnd.
 * Returns TRUE only if both hotkeys registered successfully. */
static BOOL register_hotkeys(HWND hwnd) {
    BOOL ok_full = RegisterHotKey(hwnd, HOTKEY_ID_FULL,
                                   g_cfg.hk_full_mod | MOD_NOREPEAT,
                                   g_cfg.hk_full_vk);
    if (!ok_full) {
        dbg("register_hotkeys: failed to register full-capture hotkey, GetLastError=%lu",
            GetLastError());
    }

    BOOL ok_region = RegisterHotKey(hwnd, HOTKEY_ID_REGION,
                                     g_cfg.hk_region_mod | MOD_NOREPEAT,
                                     g_cfg.hk_region_vk);
    if (!ok_region) {
        dbg("register_hotkeys: failed to register region-capture hotkey, GetLastError=%lu",
            GetLastError());
    }

    dbg("register_hotkeys: full=%s region=%s",
        ok_full ? "ok" : "FAILED", ok_region ? "ok" : "FAILED");
    return ok_full && ok_region;
}

/* Unregisters both hotkeys. Safe to call even if registration failed. */
static void unregister_hotkeys(HWND hwnd) {
    UnregisterHotKey(hwnd, HOTKEY_ID_FULL);
    UnregisterHotKey(hwnd, HOTKEY_ID_REGION);
}

/* Dispatches a WM_HOTKEY message (wParam = hotkey id) to the matching
 * capture action. Called from TrayWndProc. */
static void handle_hotkey_message(WPARAM wp) {
    switch (wp) {
        case HOTKEY_ID_FULL:
            dbg("handle_hotkey_message: full-capture hotkey pressed");
            execute_full_capture();
            break;
        case HOTKEY_ID_REGION:
            dbg("handle_hotkey_message: region-capture hotkey pressed");
            start_region_capture();
            break;
        default:
            break;
    }
}

#endif /* JXLSHOT_HOTKEYS_C */
