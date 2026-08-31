/*       https://github.com/azizr12/jxlscrenshot/        */

/* ofc this will get flag as a virus
 * because its a c code and need low level access to work
 * there is no malware or monkey
 * 
 * leave the app if you are paranoid ! !
 * trying to implement something will make this just garbage app
 * that take megabytes of binary data for a stupid key detection
 * fuck false postive and fuck modern antiviruses
 * RegisterHotKey  is shit tried it and always fail
 *
 * its just a vibe code bullshit
 * there is no malware or monkey
 * leave the app if you are paranoid
 */


/* jxlshot_tray.c — System tray extension for jxlshot.
 *
 * Configuration is read from jxlshot.ini located next to the executable.
 * Debug logs are written to %TEMP%\jxlshot_debug.log       */




#define UNICODE
#define _UNICODE
#define WINVER 0x0601
#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN
#define JXLSHOT_TRAY_BUILD

#include "resource.h"
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <windowsx.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include "jxlshot.c" // Pulls in core logic, config, and unified dbg() logger

#include <uxtheme.h>

// Undocumented but stable uxtheme APIs for Win32 Dark Mode (Windows 10 1903+)
typedef enum _PreferredAppMode {
    Default = 0,
    AllowDark = 1,
    ForceDark = 2,
    ForceLight = 3,
    Max = 4
} PreferredAppMode;

typedef PreferredAppMode (WINAPI *fnSetPreferredAppMode)(PreferredAppMode appMode);
typedef BOOL (WINAPI *fnAllowDarkModeForWindow)(HWND hWnd, BOOL allow);
typedef void (WINAPI *fnFlushMenuThemes)(void);

static void ApplyDarkMode(HWND hwnd) {
    HMODULE hUxtheme = LoadLibraryW(L"uxtheme.dll");
    if (hUxtheme) {
        // Ordinals have remained stable since Windows 10 version 1903
        fnSetPreferredAppMode pSetPreferredAppMode = (fnSetPreferredAppMode)GetProcAddress(hUxtheme, MAKEINTRESOURCEA(135));
        fnAllowDarkModeForWindow pAllowDarkModeForWindow = (fnAllowDarkModeForWindow)GetProcAddress(hUxtheme, MAKEINTRESOURCEA(133));
        fnFlushMenuThemes pFlushMenuThemes = (fnFlushMenuThemes)GetProcAddress(hUxtheme, MAKEINTRESOURCEA(136));

        if (pSetPreferredAppMode && pAllowDarkModeForWindow && pFlushMenuThemes) {
            pSetPreferredAppMode(AllowDark);
            pAllowDarkModeForWindow(hwnd, TRUE);
            pFlushMenuThemes(); // Forces existing menus to redraw with the new theme
        }
        FreeLibrary(hUxtheme);
    }
}

static HWND g_hwndMenuOwner = NULL;

#define WM_TRAYICON            (WM_USER + 1)
#define WM_HOOK_FULL_CAPTURE   (WM_USER + 10)
#define WM_HOOK_REGION_CAPTURE (WM_USER + 11)

#define ID_TRAY       1
#define IDM_FULL      101
#define IDM_REGION    102
#define IDM_SETPATH   104
#define IDM_ABOUT     105
#define IDM_RELOAD    106
#define IDM_EXIT      103

// Explicitly define the icon resource ID here to prevent "undeclared" errors in CI/CD pipelines
#define IDI_APP_ICON  1001

static NOTIFYICONDATAW g_nid;
static HWND            g_hwndTray = NULL;
static HHOOK           g_hhkKeyboard = NULL;

static void reload_config(void) { init_config(); }

/* ------------------------------------------------------------------ */
/* Tray Icon & Context Menu                                           */
/* ------------------------------------------------------------------ */
static void show_tray_menu(HWND hwnd) {
    POINT pt; GetCursorPos(&pt);
    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, IDM_FULL, L"Capture Full Screen");
    AppendMenuW(hMenu, MF_STRING, IDM_REGION, L"Capture Region...");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, IDM_SETPATH, L"Set Export Path...");
    AppendMenuW(hMenu, MF_STRING, IDM_RELOAD, L"Reload Configuration");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, IDM_ABOUT, L"About...");
    AppendMenuW(hMenu, MF_STRING, IDM_EXIT, L"Exit");
    
    // Force dark mode on the transient menu window before displaying it
    if (g_hwndMenuOwner) {
        ApplyDarkMode(g_hwndMenuOwner);
    }
    
    // Bring the hidden menu owner to the foreground so the menu inherits its theme
    if (g_hwndMenuOwner) {
        SetForegroundWindow(g_hwndMenuOwner);
    } else {
        SetForegroundWindow(hwnd);
    }
    
    // Pass g_hwndMenuOwner, NOT the message-only g_hwndTray
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwndMenuOwner ? g_hwndMenuOwner : hwnd, NULL);
    DestroyMenu(hMenu); PostMessage(hwnd, WM_NULL, 0, 0);
}

static void execute_full_capture(void) {
    Grab g;
    ZeroMemory(&g, sizeof(g));

    dbg("execute_full_capture: begin");
    if (!grab_primary_monitor(&g)) {
        dbg("execute_full_capture: grab failed, showing MessageBox");
        MessageBoxW(NULL, L"Screen capture failed.", L"jxlshot", MB_ICONERROR);
        return;
    }

    wchar_t out_path[MAX_PATH];
    build_out_path(out_path, MAX_PATH, g.is_hdr);
    dbg("execute_full_capture: writing to %ls", out_path);

    if (!save_rgb_as_jxl(g.bits, g.w, g.h, g.is_hdr, g_cfg.lossless, g_cfg.distance, out_path)) {
        dbg("execute_full_capture: save FAILED");
        MessageBoxW(NULL, L"Encoding or saving failed.", L"jxlshot", MB_ICONERROR);
    } else {
        dbg("execute_full_capture: save OK");
    }

    free_grab(&g);
}

static void execute_set_path(void) {
    BROWSEINFOW bi = { 0 };
    
    // Note: If you have a visible main window handle, use it here instead of NULL 
    // or g_hwndTray to ensure the dialog is properly modal and centered.
    bi.hwndOwner = g_hwndTray; 
    bi.lpszTitle = L"Select Export Folder for Screenshots";
    
    // BIF_USENEWUI is the modern standard (includes BIF_NEWDIALOGSTYLE | BIF_EDITBOX)
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_USENEWUI;
    
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        // Use a larger buffer (32768) to safely handle modern long paths
        wchar_t path[32768];
        
        if (SHGetPathFromIDListW(pidl, path)) {
            // Safely copy to config, ensuring null-termination (requires <stdlib.h> for _countof)
            wcsncpy_s(g_cfg.export_path, _countof(g_cfg.export_path), path, _TRUNCATE);
            
            // Safely build the INI path with guaranteed null-termination
            wchar_t ini_path[32768];
            if (_snwprintf_s(ini_path, _countof(ini_path), _TRUNCATE, L"%s\\jxlshot.ini", g_exe_dir) == 0) {
                
                WritePrivateProfileStringW(L"Capture", L"ExportPath", path, ini_path);
                MessageBoxW(g_hwndTray, L"Export path updated.", L"jxlshot", MB_ICONINFORMATION);
                
            } else {
                // Handle the rare case where the executable directory path itself is excessively long
                MessageBoxW(g_hwndTray, L"Failed to build configuration path.", L"jxlshot", MB_ICONERROR);
            }
        } else {
            MessageBoxW(g_hwndTray, L"Failed to resolve the selected folder path.", L"jxlshot", MB_ICONERROR);
        }
        
        // Correctly free the PIDL allocated by the shell
        CoTaskMemFree(pidl);
    }
}

/* ------------------------------------------------------------------ */
/* About Dialog with Clickable Hyperlink and Custom Header Icon       */
/* ------------------------------------------------------------------ */
static HRESULT CALLBACK AboutDialogCallback(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, LONG_PTR lpRefData) {
    // Intercept the exact moment the Task Dialog window is created
    if (msg == TDN_CREATED) {
        // Use the cached initialization function from the previous refactoring
        InitDarkModeAPI(); 
        
        if (pAllowDarkModeForWindow) {
            // Apply dark mode DIRECTLY to the Task Dialog's HWND
            pAllowDarkModeForWindow(hwnd, TRUE);
        }
        if (pFlushMenuThemes) {
            pFlushMenuThemes();
        }
    }
    
    if (msg == TDN_HYPERLINK_CLICKED) {
        // Check return value to ensure the shell execute was successful (> 32)
        HINSTANCE hResult = ShellExecuteW(hwnd, L"open", (LPCWSTR)lParam, NULL, NULL, SW_SHOWNORMAL);
        if ((INT_PTR)hResult <= 32) {
            // Optional: Handle failure (e.g., no default browser configured)
            MessageBoxW(hwnd, L"Failed to open the link. Please check your default browser settings.", L"jxlshot", MB_ICONWARNING);
        }
    }
    
    return S_OK;
}

static void execute_about(void) {
    // LoadIconW returns a shared icon. Do NOT call DestroyIcon on it.
    HICON hAppIcon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_APP_ICON));

    TASKDIALOGCONFIG config = {0};
    config.cbSize = sizeof(TASKDIALOGCONFIG);
    
    // SAFETY FIX: Avoid using message-only windows as parents for modal dialogs.
    // GetForegroundWindow() is the safest bet for a tray app to ensure proper z-order and focus.
    // Fallback to NULL if no foreground window is available.
    HWND hParent = GetForegroundWindow();
    if (hParent == NULL || IsWindowVisible(hParent) == FALSE) {
        hParent = NULL; 
    }
    config.hwndParent = hParent;
    
    config.hInstance = NULL;
    config.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_USE_HICON_MAIN;
    config.pszWindowTitle = L"About";
    config.pszMainIcon = (PCWSTR)hAppIcon; 
    config.pszMainInstruction = L"JXL Screenshot Tool";
    config.pszContent = L"Minimal tray screenshot tool using JPEG XL.\n\n"
                        L"<a href=\"https://github.com/azizr12/jxlscrenshot\">https://github.com/azizr12/jxlscrenshot</a>";
    config.pfCallback = AboutDialogCallback;

    TaskDialogIndirect(&config, NULL, NULL, NULL);

    // REMOVED: DestroyIcon(hAppIcon); 
    // LoadIconW returns a shared system resource. Destroying it is unsafe.
}
/* ------------------------------------------------------------------ */
/* Interactive Region Selection (Optimized & Ghosting Fixed)          */
/* ------------------------------------------------------------------ */
static HWND    g_hwndRegion = NULL;
static HDC     g_hdcMem = NULL, g_hdcBlack = NULL;
static HBITMAP g_hbmScreen = NULL, g_hbmBlack = NULL;
static int     g_screenW, g_screenH;
static RECT    g_rcSel;
static BOOL    g_isDragging = FALSE;

static void crop_and_encode_region(RECT *r) {
    Grab g;
    ZeroMemory(&g, sizeof(g));
    
    if (!grab_primary_monitor(&g)) {
        return;
    }
    
    int rw = r->right - r->left;
    int rh = r->bottom - r->top;
    
    if (rw <= 0 || rh <= 0) {
        free_grab(&g);
        return;
    }

    size_t bytes_per_pixel = g.is_hdr ? 6 : 3;
    
    // SAFETY: Verify that the source buffer is tightly packed. 
    // If grab_primary_monitor uses a stride (e.g., padded to 4 bytes), 
    // you must use g.stride instead of (g.w * bytes_per_pixel) here.
    size_t src_stride = g.w * bytes_per_pixel; // REPLACE with g.stride if applicable
    
    uint8_t *crop_bits = (uint8_t *)malloc((size_t)rw * rh * bytes_per_pixel);
    if (!crop_bits) {
        free_grab(&g);
        return;
    }
    
    // Copy row by row, accounting for potential source stride
    size_t dst_stride = (size_t)rw * bytes_per_pixel;
    for (int y = 0; y < rh; y++) {
        memcpy(
            crop_bits + (size_t)y * dst_stride, 
            g.bits + ((size_t)(r->top + y) * g.w + r->left) * bytes_per_pixel, // Update if using src_stride
            dst_stride
        );
    }
    
    wchar_t out_path[32768]; // Upgraded to support long paths
    build_out_path(out_path, _countof(out_path), g.is_hdr); 
    
    save_rgb_as_jxl(crop_bits, rw, rh, g.is_hdr, g_cfg.lossless, g_cfg.distance, out_path);
    
    free(crop_bits);
    free_grab(&g);
}

LRESULT CALLBACK RegionWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps; 
            HDC hdc = BeginPaint(hwnd, &ps);
            
            // 1. Restore the original screen capture
            BitBlt(hdc, 0, 0, g_screenW, g_screenH, g_hdcMem, 0, 0, SRCCOPY);
            
            // 2. Apply dark mode overlay (Alpha 120)
            BLENDFUNCTION bf = { AC_SRC_OVER, 0, 120, 0 };
            AlphaBlend(hdc, 0, 0, g_screenW, g_screenH, g_hdcBlack, 0, 0, 1, 1, bf);

            // 3. Draw the selection area and border
            if (g_isDragging || (g_rcSel.right > g_rcSel.left && g_rcSel.bottom > g_rcSel.top)) {
                int x = min(g_rcSel.left, g_rcSel.right);
                int y = min(g_rcSel.top, g_rcSel.bottom);
                int w = abs(g_rcSel.right - g_rcSel.left);
                int h = abs(g_rcSel.bottom - g_rcSel.top);
                
                // Restore the clear image in the selected region
                BitBlt(hdc, x, y, w, h, g_hdcMem, x, y, SRCCOPY);
                
                HPEN hPen = CreatePen(PS_SOLID, 2, RGB(0, 120, 215));
                HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
                HBRUSH hOldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
                Rectangle(hdc, x, y, x + w, y + h);
                
                SelectObject(hdc, hOldBrush); 
                SelectObject(hdc, hOldPen); 
                DeleteObject(hPen);

                // 4. Dimension Tooltip
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, RGB(255, 255, 255));
                
                wchar_t dimText[64];
                // SECURE: Use _snwprintf_s to guarantee null-termination
                _snwprintf_s(dimText, _countof(dimText), _TRUNCATE, L"%d × %d", w, h);
                
                int textY = (y - 24) < 0 ? (y + 8) : (y - 24);
                TextOutW(hdc, x + 8, textY, dimText, (int)wcslen(dimText));
            }
            
            EndPaint(hwnd, &ps); 
            return 0;
        }

        case WM_LBUTTONDOWN: {
            g_isDragging = TRUE; 
            g_rcSel.left = GET_X_LPARAM(lp); 
            g_rcSel.top = GET_Y_LPARAM(lp);
            g_rcSel.right = g_rcSel.left; 
            g_rcSel.bottom = g_rcSel.top;
            SetCapture(hwnd); 
            InvalidateRect(hwnd, NULL, FALSE); 
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (g_isDragging) {
                g_rcSel.right = GET_X_LPARAM(lp); 
                g_rcSel.bottom = GET_Y_LPARAM(lp);
                InvalidateRect(hwnd, NULL, FALSE);
            } 
            return 0;
        }
        case WM_LBUTTONUP: {
            if (g_isDragging) {
                g_isDragging = FALSE; 
                ReleaseCapture();
        
                RECT r = g_rcSel;
                if (r.left > r.right) { int t = r.left; r.left = r.right; r.right = t; }
                if (r.top > r.bottom) { int t = r.top; r.top = r.bottom; r.bottom = t; }
        
                ShowWindow(hwnd, SW_HIDE);
                DestroyWindow(hwnd);
                
                // PERFORMANCE FIX: Removed RDW_ALLCHILDREN. 
                // Let DWM naturally repaint the desktop. If ghosting occurs, 
                // invalidate only the specific region, not the entire OS.
                // RedrawWindow(NULL, &r, NULL, RDW_INVALIDATE | RDW_UPDATENOW); // Optional fallback
                
                if ((r.right - r.left) > 0 && (r.bottom - r.top) > 0) {
                    crop_and_encode_region(&r);
                }
            } 
            return 0;
        }
        case WM_KEYDOWN: {
            if (wp == VK_ESCAPE) {
                g_isDragging = FALSE; 
                ReleaseCapture();
        
                ShowWindow(hwnd, SW_HIDE);
                DestroyWindow(hwnd);
                
                // Same performance fix as above: do not use RDW_ALLCHILDREN
            } 
            return 0;
        }
        case WM_DESTROY: {
            if (g_hdcBlack) { DeleteDC(g_hdcBlack); g_hdcBlack = NULL; }
            if (g_hbmBlack) { DeleteObject(g_hbmBlack); g_hbmBlack = NULL; }
            if (g_hdcMem)   { DeleteDC(g_hdcMem); g_hdcMem = NULL; }
            if (g_hbmScreen){ DeleteObject(g_hbmScreen); g_hbmScreen = NULL; }
            
            g_hwndRegion = NULL;
            g_isDragging = FALSE;
            g_rcSel.left = g_rcSel.top = g_rcSel.right = g_rcSel.bottom = 0;
            
            // Ensure cursor is restored. Consider tracking a local boolean 
            // to ensure ShowCursor(FALSE) was actually called before this.
            ShowCursor(TRUE); 
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void start_region_capture(void) {
    if (g_hwndRegion) return;

    // Ensure a completely clean slate before starting a new capture
    g_isDragging = FALSE;
    g_rcSel.left = g_rcSel.top = g_rcSel.right = g_rcSel.bottom = 0;

    // MULTI-MONITOR FIX: Use virtual screen metrics to cover all displays
    int screenX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int screenY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    g_screenW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    g_screenH = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    HDC hdcScreen = GetDC(NULL);
    if (!hdcScreen) return;

    g_hdcMem = CreateCompatibleDC(hdcScreen);
    if (!g_hdcMem) goto cleanup_dc;

    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = g_screenW;
    bi.bmiHeader.biHeight = -g_screenH; // Top-down DIB
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    // WYSIWYG IMPROVEMENT: Capture the pointer to the bitmap bits
    // This allows crop_and_encode_region to read directly from this buffer
    // instead of calling grab_primary_monitor() again.
    void* pScreenBits = NULL;
    g_hbmScreen = CreateDIBSection(hdcScreen, &bi, DIB_RGB_COLORS, &pScreenBits, NULL, 0);
    if (!g_hbmScreen) goto cleanup_mem_dc;
    
    SelectObject(g_hdcMem, g_hbmScreen);
    BitBlt(g_hdcMem, 0, 0, g_screenW, g_screenH, hdcScreen, 0, 0, SRCCOPY);

    g_hdcBlack = CreateCompatibleDC(hdcScreen);
    if (!g_hdcBlack) goto cleanup_dib;

    g_hbmBlack = CreateCompatibleBitmap(hdcScreen, 1, 1);
    if (!g_hbmBlack) goto cleanup_black_dc;

    HBITMAP hOldBmp = (HBITMAP)SelectObject(g_hdcBlack, g_hbmBlack);
    RECT rc = {0, 0, 1, 1};
    FillRect(g_hdcBlack, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
    SelectObject(g_hdcBlack, hOldBmp);

    ReleaseDC(NULL, hdcScreen);

    // CLASS REGISTRATION: Guarded to prevent redundant calls
    static BOOL classRegistered = FALSE;
    if (!classRegistered) {
        WNDCLASSEXW wc = {0};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = RegionWndProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, IDC_CROSS);
        wc.lpszClassName = L"JxlShotRegionClass";
        
        if (RegisterClassExW(&wc)) {
            classRegistered = TRUE;
        } else {
            goto cleanup_black_bmp; // Fatal: cannot create window without class
        }
    }

    g_hwndRegion = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW, 
        L"JxlShotRegionClass", 
        L"",
        WS_POPUP, 
        screenX, screenY, g_screenW, g_screenH, 
        NULL, NULL, GetModuleHandleW(NULL), NULL
    );

    if (!g_hwndRegion) {
        goto cleanup_black_bmp;
    }

    if (!g_cfg.show_cursor) {
        ShowCursor(FALSE);
    }
    
    ShowWindow(g_hwndRegion, SW_SHOW);
    UpdateWindow(g_hwndRegion);
    return;

/* ------------------------------------------------------------------ */
/* Centralized Cleanup Path for Initialization Failures               */
/* ------------------------------------------------------------------ */
cleanup_black_bmp:
    if (g_hbmBlack) { DeleteObject(g_hbmBlack); g_hbmBlack = NULL; }
cleanup_black_dc:
    if (g_hdcBlack) { DeleteDC(g_hdcBlack); g_hdcBlack = NULL; }
cleanup_dib:
    if (g_hbmScreen) { DeleteObject(g_hbmScreen); g_hbmScreen = NULL; }
cleanup_mem_dc:
    if (g_hdcMem) { DeleteDC(g_hdcMem); g_hdcMem = NULL; }
cleanup_dc:
    ReleaseDC(NULL, hdcScreen);
}

/* ------------------------------------------------------------------ */
/* Low-Level Keyboard Hook                                            */
/* ------------------------------------------------------------------ */

/* Helper function to verify if the required modifier keys are currently pressed */
/* ------------------------------------------------------------------ */
/* Low-Level Keyboard Hook (Strict INI Enforcement)                   */
/* ------------------------------------------------------------------ */

/* Helper function to verify EXACT modifier match. 
 * If the INI requires Ctrl, Ctrl must be pressed. 
 * If the INI does NOT require Ctrl, Ctrl must NOT be pressed. */

static BOOL check_modifiers(UINT required_mod) {
    BOOL ctrl_pressed = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    BOOL shift_pressed = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    BOOL alt_pressed = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    BOOL win_pressed = (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 || (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;

    BOOL req_ctrl = (required_mod & MOD_CONTROL) != 0;
    BOOL req_shift = (required_mod & MOD_SHIFT) != 0;
    BOOL req_alt = (required_mod & MOD_ALT) != 0;
    BOOL req_win = (required_mod & MOD_WIN) != 0;

    // All required modifiers must match the pressed state exactly
    if (ctrl_pressed != req_ctrl) return FALSE;
    if (shift_pressed != req_shift) return FALSE;
    if (alt_pressed != req_alt) return FALSE;
    if (win_pressed != req_win) return FALSE;

    return TRUE;
}

static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && wParam == WM_KEYDOWN) {
        KBDLLHOOKSTRUCT *pKB = (KBDLLHOOKSTRUCT *)lParam;
        
        /* 1. Check Full Capture Hotkey 
         * Condition: Key is defined (!= 0) AND pressed key matches AND modifiers match exactly */
        if (g_cfg.hk_full_vk != 0 && pKB->vkCode == g_cfg.hk_full_vk) {
            if (check_modifiers(g_cfg.hk_full_mod)) {
                dbg("MATCH: Triggering FULL capture (VK=%d, MOD=%d)", g_cfg.hk_full_vk, g_cfg.hk_full_mod);
                PostMessageW(g_hwndTray, WM_HOOK_FULL_CAPTURE, 0, 0);
                return 1; // Block key from propagating to other apps
            }
        }
        
        /* 2. Check Region Capture Hotkey 
         * Condition: Key is defined (!= 0) AND pressed key matches AND modifiers match exactly */
        if (g_cfg.hk_region_vk != 0 && pKB->vkCode == g_cfg.hk_region_vk) {
            if (check_modifiers(g_cfg.hk_region_mod)) {
                dbg("MATCH: Triggering REGION capture (VK=%d, MOD=%d)", g_cfg.hk_region_vk, g_cfg.hk_region_mod);
                PostMessageW(g_hwndTray, WM_HOOK_REGION_CAPTURE, 0, 0);
                return 1; // Block key from propagating to other apps
            }
        }
    }
    
    // If no match, or if the configured VK is 0 (null/disabled), pass the key to the OS normally
    return CallNextHookEx(g_hhkKeyboard, nCode, wParam, lParam);
}

static void install_keyboard_hook(void) {
    if (g_hhkKeyboard) return;
    g_hhkKeyboard = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandle(NULL), 0);
}

static void uninstall_keyboard_hook(void) {
    if (g_hhkKeyboard) { UnhookWindowsHookEx(g_hhkKeyboard); g_hhkKeyboard = NULL; }
}

/* ------------------------------------------------------------------ */
/* Tray Window Procedure & Entry Point                                */
/* ------------------------------------------------------------------ */
LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lParam) {
    switch (msg) {
        case WM_TRAYICON:
            if (lParam == WM_RBUTTONUP || lParam == WM_LBUTTONUP) show_tray_menu(hwnd);
            break;
        case WM_HOOK_FULL_CAPTURE: execute_full_capture(); break;
        case WM_HOOK_REGION_CAPTURE: start_region_capture(); break;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDM_FULL: execute_full_capture(); break;
                case IDM_REGION: start_region_capture(); break;
                case IDM_SETPATH: execute_set_path(); break;
                case IDM_RELOAD: reload_config(); break;
                case IDM_ABOUT: execute_about(); break;
                case IDM_EXIT: PostQuitMessage(0); break;
            } break;
        case WM_DESTROY:
            uninstall_keyboard_hook();
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            if (g_hwndMenuOwner) {
                DestroyWindow(g_hwndMenuOwner);
                g_hwndMenuOwner = NULL;
            }
            PostQuitMessage(0); break;
        default: return DefWindowProcW(hwnd, msg, wp, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR szCmdLine, int sw) {
    set_dpi_aware(); 
    init_paths(); 
    ensure_default_ini(); 
    init_config();
    dbg_init();

    WNDCLASSEXW wc = {0}; 
    wc.cbSize = sizeof(wc); 
    wc.lpfnWndProc = TrayWndProc;
    wc.hInstance = hInst; 
    wc.lpszClassName = L"JxlShotTrayClass"; 
    RegisterClassExW(&wc);
    
    g_hwndTray = CreateWindowExW(0, L"JxlShotTrayClass", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hInst, NULL);
    
    // Create a hidden popup window specifically to own the context menu
    g_hwndMenuOwner = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        L"JxlShotTrayClass",
        L"", 
        WS_POPUP,
        0, 0, 0, 0,
        NULL, NULL, hInst, NULL
    );
    ShowWindow(g_hwndMenuOwner, SW_HIDE);
    ApplyDarkMode(g_hwndMenuOwner);
    
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid); 
    g_nid.hWnd = g_hwndTray; 
    g_nid.uID = ID_TRAY;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP; 
    g_nid.uCallbackMessage = WM_TRAYICON;
    
    g_nid.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APP_ICON)); 
    wcscpy(g_nid.szTip, L"JXL Screenshot Tool");
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    install_keyboard_hook();

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) { 
        TranslateMessage(&msg); 
        DispatchMessage(&msg); 
    }
    
    uninstall_keyboard_hook();
    return (int)msg.wParam;
}
