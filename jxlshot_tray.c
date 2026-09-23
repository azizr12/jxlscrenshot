//       https://github.com/azizr12/jxlscrenshot/

/* ofc this will get flag as a virus
 * because its a c code and need low level access to work
 * there is no malware or shit
 * 
 * leave the app if you are paranoid ! !
 * trying to implement something will make this just garbage app
 * that take megabytes of binary data for a stupid key detection
 * fuck false postive and fuck modern antiviruses
 * RegisterHotKey  is shit tried it and always fail
 *
 * its just a vibe code bullshit
 * leave the app if you are paranoid
 */


/* jxlshot_tray.c — System tray extension for jxlshot.
 *
 * Configuration is read from jxlshot.ini located next to the executable.
 * Debug logs are written to %TEMP%\jxlshot_debug.log
 */




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
#include <urlmon.h>
#pragma comment(lib, "urlmon.lib") // (For MSVC)
#pragma comment(lib, "comctl32.lib")

// Undocumented but stable uxtheme APIs for Win32 Dark Mode (Windows 10 1903+)

// Windows Dark Mode Support


typedef enum _PreferredAppMode {
    Default    = 0,
    AllowDark  = 1,
    ForceDark  = 2,
    ForceLight = 3,
    Max        = 4
} PreferredAppMode;

typedef PreferredAppMode (WINAPI *fnSetPreferredAppMode)(PreferredAppMode appMode);
typedef BOOL (WINAPI *fnAllowDarkModeForWindow)(HWND hWnd, BOOL allow);
typedef void (WINAPI *fnFlushMenuThemes)(void);

// Global state to avoid reloading the DLL repeatedly
static HMODULE g_hUxtheme = NULL;
static fnSetPreferredAppMode g_pSetPreferredAppMode = NULL;
static fnAllowDarkModeForWindow g_pAllowDarkModeForWindow = NULL;
static fnFlushMenuThemes g_pFlushMenuThemes = NULL;

static void InitializeDarkMode(void) {
    if (g_hUxtheme) return;

    g_hUxtheme = LoadLibraryW(L"uxtheme.dll");
    if (!g_hUxtheme) return;

    g_pSetPreferredAppMode = (fnSetPreferredAppMode)GetProcAddress(g_hUxtheme, MAKEINTRESOURCEA(135));
    g_pAllowDarkModeForWindow = (fnAllowDarkModeForWindow)GetProcAddress(g_hUxtheme, MAKEINTRESOURCEA(133));
    g_pFlushMenuThemes = (fnFlushMenuThemes)GetProcAddress(g_hUxtheme, MAKEINTRESOURCEA(136));

    // ForceDark is required for TaskDialogs to reliably apply the theme
    if (g_pSetPreferredAppMode) {
        g_pSetPreferredAppMode(ForceDark);
    }
    if (g_pFlushMenuThemes) {
        g_pFlushMenuThemes();
    }
}

static void ApplyDarkMode(HWND hwnd) {
    InitializeDarkMode();
    if (!hwnd || !g_pAllowDarkModeForWindow) return;

    g_pAllowDarkModeForWindow(hwnd, TRUE);
    
    // Crucial: Tell the window and its children to redraw with the new theme
    SendMessageW(hwnd, WM_THEMECHANGED, 0, 0);
    RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
}

static HWND g_hwndMenuOwner = NULL;

#define WM_TRAYICON            (WM_USER + 1)
#define WM_HOOK_FULL_CAPTURE   (WM_USER + 10)
#define WM_HOOK_REGION_CAPTURE (WM_USER + 11)

#define ID_TRAY          1
#define IDM_FULL         101
#define IDM_REGION       102
#define IDM_SETPATH      104
#define IDM_ABOUT        105
#define IDM_RELOAD       106
#define IDM_EXIT         103
#define IDM_OPENCONFIG   107
#define IDM_CHECK_UPDATE 108
#define IDM_OPENEXPORT   109

// Explicitly define the icon resource ID here to prevent "undeclared" errors in CI/CD pipelines
#define IDI_APP_ICON  1001

static NOTIFYICONDATAW g_nid;
static HWND            g_hwndTray = NULL;
static HHOOK           g_hhkKeyboard = NULL;
static HHOOK           g_hhkMouse = NULL;
static BOOL            g_isRegionCapturing = FALSE;
static DWORD           g_regionCaptureEndTime = 0;

// Forward declarations to fix implicit declaration errors
static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam);
static void install_mouse_hook(void);
static void uninstall_mouse_hook(void);

static void reload_config(void) { init_config(); }



// Tray Icon & Context Menu



static void show_tray_menu(HWND hwnd) {
    POINT pt; GetCursorPos(&pt);
    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, IDM_FULL, L"Capture Full Screen");
    AppendMenuW(hMenu, MF_STRING, IDM_REGION, L"Capture Region...");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, IDM_SETPATH, L"Set Export Path...");
    AppendMenuW(hMenu, MF_STRING, IDM_OPENEXPORT, L"Open Export Folder");
    AppendMenuW(hMenu, MF_STRING, IDM_OPENCONFIG, L"Open Config File");
    AppendMenuW(hMenu, MF_STRING, IDM_RELOAD, L"Reload Configuration");
    AppendMenuW(hMenu, MF_STRING, IDM_CHECK_UPDATE, L"Check for Updates...");
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

    // OFFLOAD TO BACKGROUND THREAD
    EncodeTask *task = (EncodeTask *)malloc(sizeof(EncodeTask));
    if (task) {
        task->bits = g.bits;
        task->w = g.w;
        task->h = g.h;
        task->is_hdr = g.is_hdr;
        task->lossless = g_cfg.lossless;
        task->distance = g_cfg.distance;
        wcsncpy_s(task->out_path, MAX_PATH, out_path, _TRUNCATE);
        
        HANDLE hThread = CreateThread(NULL, 0, EncodeWorker, task, 0, NULL);
        if (hThread) {
            CloseHandle(hThread);
            g.bits = NULL; // Thread owns the buffer now
            dbg("execute_full_capture: encoding offloaded to background thread");
        } else {
            dbg("execute_full_capture: CreateThread failed, falling back to synchronous");
            if (!save_rgb_as_jxl(task->bits, task->w, task->h, task->is_hdr, task->lossless, task->distance, task->out_path)) {
                MessageBoxW(NULL, L"Encoding or saving failed.", L"jxlshot", MB_ICONERROR);
            }
            free(task->bits);
            free(task);
            g.bits = NULL;
        }
    } else {
        dbg("execute_full_capture: malloc failed, falling back to synchronous");
        if (!save_rgb_as_jxl(g.bits, g.w, g.h, g.is_hdr, g_cfg.lossless, g_cfg.distance, out_path)) {
            MessageBoxW(NULL, L"Encoding or saving failed.", L"jxlshot", MB_ICONERROR);
        }
    }

    free_grab(&g); 
}



// Version Parsing Helpers



typedef struct { int major, minor, patch; } Version;

static Version parse_version(const wchar_t* str) {
    Version v = {0, 0, 0};
    if (str) {
        swscanf(str, L"%d.%d.%d", &v.major, &v.minor, &v.patch);
    }
    return v;
}

static int compare_versions(Version a, Version b) {
    if (a.major != b.major) return a.major - b.major;
    if (a.minor != b.minor) return a.minor - b.minor;
    return a.patch - b.patch;
}


static void execute_check_update(HWND hwnd) {
    // Cache-busting URL to guarantee a fresh download every time
    wchar_t remote_url[512];
    _snwprintf(remote_url, 512, L"https://raw.githubusercontent.com/azizr12/jxlscrenshot/main/VERSION?t=%llu", (unsigned long long)GetTickCount64());
    
    wchar_t temp_path[MAX_PATH];
    GetTempPathW(MAX_PATH, temp_path);
    wcscat_s(temp_path, MAX_PATH, L"jxlshot_version.txt");

    // Download the file synchronously
    HRESULT hr = URLDownloadToFileW(NULL, remote_url, temp_path, 0, NULL);
    
    if (FAILED(hr)) {
        MessageBoxW(hwnd, L"Failed to connect to the update server.\nPlease check your internet connection.", L"Update Check", MB_ICONWARNING | MB_OK);
        return;
    }

    HANDLE hFile = CreateFileW(temp_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        MessageBoxW(hwnd, L"Failed to read update information.", L"Update Check", MB_ICONERROR | MB_OK);
        return;
    }

    char buffer[64] = {0};
    DWORD bytes_read = 0;
    ReadFile(hFile, buffer, sizeof(buffer) - 1, &bytes_read, NULL);
    CloseHandle(hFile);
    DeleteFileW(temp_path); // Clean up temp file immediately

    if (bytes_read == 0) {
        MessageBoxW(hwnd, L"Update server returned empty data.", L"Update Check", MB_ICONERROR | MB_OK);
        return;
    }

    wchar_t remote_version_str[64] = {0};
    MultiByteToWideChar(CP_UTF8, 0, buffer, -1, remote_version_str, 64);
    
    // Trim trailing whitespace/newlines
    for (int i = wcslen(remote_version_str) - 1; i >= 0; i--) {
        if (remote_version_str[i] == L'\r' || remote_version_str[i] == L'\n' || remote_version_str[i] == L' ') {
            remote_version_str[i] = L'\0';
        } else {
            break;
        }
    }

    // Parse and compare versions
    Version remote_v = parse_version(remote_version_str);
    Version current_v = parse_version(APP_VERSIONW);

    if (compare_versions(remote_v, current_v) > 0) {
        wchar_t msg[256];
        _snwprintf(msg, 256, L"A new version is available!\n\nCurrent Version: %s\nLatest Version:  %s\n\nWould you like to open the releases page?", APP_VERSIONW, remote_version_str);
        
        int result = MessageBoxW(hwnd, msg, L"Update Available", MB_ICONINFORMATION | MB_YESNO);
        if (result == IDYES) {
            ShellExecuteW(hwnd, L"open", L"https://github.com/azizr12/jxlscrenshot/releases/latest", NULL, NULL, SW_SHOWNORMAL);
        }
    } else {
        MessageBoxW(hwnd, L"You are already using the latest version.", L"Update Check", MB_ICONINFORMATION | MB_OK);
    }
}

static void execute_set_path(void) {
    BROWSEINFOW bi = { 0 }; bi.hwndOwner = NULL;
    bi.lpszTitle = L"Select Export Folder for Screenshots";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        wchar_t path[MAX_PATH];
        if (SHGetPathFromIDListW(pidl, path)) {
            wcsncpy(g_cfg.export_path, path, MAX_PATH - 1); g_cfg.export_path[MAX_PATH - 1] = L'\0';
            wchar_t ini_path[MAX_PATH]; _snwprintf(ini_path, MAX_PATH, L"%s\\jxlshot.ini", g_exe_dir);
            WritePrivateProfileStringW(L"Capture", L"ExportPath", path, ini_path);
            MessageBoxW(NULL, L"Export path updated.", L"jxlshot", MB_ICONINFORMATION);
        }
        CoTaskMemFree(pidl);
    }
}

static void execute_open_config(HWND hwnd) {
    wchar_t ini_path[MAX_PATH];
    _snwprintf(ini_path, MAX_PATH, L"%s\\jxlshot.ini", g_exe_dir);
    
    // Attempt 1: Explicitly open with Notepad
    HINSTANCE hResult = ShellExecuteW(hwnd, L"open", L"notepad.exe", ini_path, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)hResult > 32) {
        return; // Success
    }
    
    // Attempt 2: Fallback to default .txt/.ini viewer
    hResult = ShellExecuteW(hwnd, L"open", ini_path, NULL, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)hResult > 32) {
        return; // Success
    }
    
    // Attempt 3: Fallback to simply opening the root folder containing the INI
    ShellExecuteW(hwnd, L"explore", g_exe_dir, NULL, NULL, SW_SHOWNORMAL);
}

static void execute_open_export_folder(void) {
    wchar_t path_to_open[MAX_PATH];
    
    // Use the configured export path, or fall back to the executable directory
    if (g_cfg.export_path[0] != L'\0') {
        wcsncpy_s(path_to_open, MAX_PATH, g_cfg.export_path, _TRUNCATE);
    } else {
        wcsncpy_s(path_to_open, MAX_PATH, g_exe_dir, _TRUNCATE);
    }
    
    // Open the folder in Windows Explorer
    ShellExecuteW(NULL, L"explore", path_to_open, NULL, NULL, SW_SHOWNORMAL);
}





// About Dialog with Clickable Hyperlink and Custom Header Icon



static HRESULT CALLBACK AboutDialogCallback(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, LONG_PTR lpRefData) {
    if (msg == TDN_CREATED) {
        // Apply dark mode to the TaskDialog as soon as it's created
        ApplyDarkMode(hwnd);
    }
    
    if (msg == TDN_HYPERLINK_CLICKED) {
        ShellExecuteW(hwnd, L"open", (LPCWSTR)lParam, NULL, NULL, SW_SHOWNORMAL);
    }
    return S_OK;
}

static void execute_about(void) {
    HICON hAppIcon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_APP_ICON));

    TASKDIALOGCONFIG config = {0};
    config.cbSize = sizeof(TASKDIALOGCONFIG);
    
    // Temporarily make the parent window "visible" to the OS 
    // window manager without activating it. This prevents the TaskDialog 
    // from getting lost behind other applications, while still allowing it 
    // to inherit the dark mode theme from g_hwndMenuOwner.
    ShowWindow(g_hwndMenuOwner, SW_SHOWNOACTIVATE);
    
    config.hwndParent = g_hwndMenuOwner;
    config.hInstance = NULL;
    config.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_USE_HICON_MAIN;
    config.pszWindowTitle = L"About";
    config.pszMainIcon = (PCWSTR)hAppIcon; 
    config.pszMainInstruction = L"JXL Screenshot Tool";
    config.pszContent = L"Minimal tray screenshot tool using JPEG XL.\n\n"
                        L"<a href=\"https://github.com/azizr12/jxlscrenshot\">https://github.com/azizr12/jxlscrenshot</a>";
    config.pfCallback = AboutDialogCallback;

    // Show the dialog (it will now behave as a proper top-level window)
    TaskDialogIndirect(&config, NULL, NULL, NULL);

    // Hide the parent window again after the dialog is closed
    ShowWindow(g_hwndMenuOwner, SW_HIDE);

    if (hAppIcon) {
        DestroyIcon(hAppIcon);
    }
}



// Interactive Region Selection



static HWND    g_hwndRegion = NULL;
static HDC     g_hdcMem = NULL, g_hdcBlack = NULL;
static HBITMAP g_hbmScreen = NULL, g_hbmBlack = NULL;
static int     g_screenW, g_screenH;
static RECT    g_rcSel;
static BOOL    g_isDragging = FALSE;

static void crop_and_encode_region(RECT *r) {
    Grab g;
    ZeroMemory(&g, sizeof(g)); // Ensure clean initial state
    
    if (!grab_primary_monitor(&g)) {
        return; // Early exit is safe
    }
    
    int rw = r->right - r->left;
    int rh = r->bottom - r->top;
    
    if (rw <= 0 || rh <= 0) {
        free_grab(&g); // Guaranteed cleanup before early exit
        return;
    }

    // Determine bytes per pixel based on HDR (6 bytes) or SDR (3 bytes)
    size_t bytes_per_pixel = g.is_hdr ? 6 : 3;
    
    uint8_t *crop_bits = (uint8_t *)malloc((size_t)rw * rh * bytes_per_pixel);
    if (!crop_bits) {
        free_grab(&g); // Guaranteed cleanup before early exit
        return;
    }
    
    // Copy row by row from the tightly packed source buffer
    for (int y = 0; y < rh; y++) {
        memcpy(
            crop_bits + (size_t)y * rw * bytes_per_pixel, 
            g.bits + ((size_t)(r->top + y) * g.w + r->left) * bytes_per_pixel, 
            rw * bytes_per_pixel
        );
    }
    
    wchar_t out_path[MAX_PATH]; 
    build_out_path(out_path, MAX_PATH, g.is_hdr); 
    
    // UPDATED: Use the new identity save function and pass g.is_hdr
    // (Now offloaded to background thread to prevent UI/game freezing)
    EncodeTask *task = (EncodeTask *)malloc(sizeof(EncodeTask));
    if (task) {
        task->bits = crop_bits; // Transfer ownership of crop_bits to the thread
        task->w = rw;
        task->h = rh;
        task->is_hdr = g.is_hdr;
        task->lossless = g_cfg.lossless;
        task->distance = g_cfg.distance;
        wcsncpy_s(task->out_path, MAX_PATH, out_path, _TRUNCATE);
        
        HANDLE hThread = CreateThread(NULL, 0, EncodeWorker, task, 0, NULL);
        if (hThread) {
            CloseHandle(hThread);
            // crop_bits is owned by the thread now; do NOT free it here.
            dbg("crop_and_encode_region: encoding offloaded to background thread");
        } else {
            // Fallback to synchronous if thread creation fails
            dbg("crop_and_encode_region: CreateThread failed, falling back to synchronous");
            save_rgb_as_jxl(task->bits, task->w, task->h, task->is_hdr, task->lossless, task->distance, task->out_path);
            free(task->bits); // Guaranteed heap cleanup on fallback
            free(task);
        }
    } else {
        // Fallback if malloc fails
        dbg("crop_and_encode_region: malloc failed for EncodeTask, falling back to synchronous");
        save_rgb_as_jxl(crop_bits, rw, rh, g.is_hdr, g_cfg.lossless, g_cfg.distance, out_path);
        free(crop_bits); // Guaranteed heap cleanup on fallback
    }

    free_grab(&g);   // Guaranteed cleanup
}

LRESULT CALLBACK RegionWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps; 
            HDC hdc = BeginPaint(hwnd, &ps);
            
            // 1. Restore the original screen capture
            BitBlt(hdc, 0, 0, g_screenW, g_screenH, g_hdcMem, 0, 0, SRCCOPY);
            
            // 2. Apply dark mode overlay (Alpha 120 provides a modern dimmed effect)
            BLENDFUNCTION bf = { AC_SRC_OVER, 0, 120, 0 };
            AlphaBlend(hdc, 0, 0, g_screenW, g_screenH, g_hdcBlack, 0, 0, 1, 1, bf);

            // 3. Draw the selection area and border
            if (g_isDragging || (g_rcSel.right > g_rcSel.left && g_rcSel.bottom > g_rcSel.top)) {
                int x = min(g_rcSel.left, g_rcSel.right);
                int y = min(g_rcSel.top, g_rcSel.bottom);
                int w = abs(g_rcSel.right - g_rcSel.left);
                int h = abs(g_rcSel.bottom - g_rcSel.top);
                
                // Restore the clear (non-dimmed) image in the selected region
                BitBlt(hdc, x, y, w, h, g_hdcMem, x, y, SRCCOPY);
                
                // Modern Accent Color Border (Windows Blue: #0078D7)
                HPEN hPen = CreatePen(PS_SOLID, 2, RGB(0, 120, 215));
                HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
                HBRUSH hOldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
                Rectangle(hdc, x, y, x + w, y + h);
                
                // Clean up GDI objects properly
                SelectObject(hdc, hOldBrush); 
                SelectObject(hdc, hOldPen); 
                DeleteObject(hPen);

                // 4. Dimension Tooltip (White text, transparent background)
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, RGB(255, 255, 255));
                
                wchar_t dimText[64];
                _snwprintf(dimText, 64, L"%d × %d", w, h);
                
                // Prevent tooltip from drawing off the top edge of the screen
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
                
                // Force desktop to repaint immediately to clear any ghosting artifacts
                RedrawWindow(NULL, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_ERASE);

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
                
                // Force desktop to repaint immediately to clear any ghosting artifacts
                RedrawWindow(NULL, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_ERASE);
            } 
            return 0;
        }
        
        /* Right-Click Cancellation */
        case WM_RBUTTONDOWN: {
            // Right-click cancels the selection, exactly like the Escape key
            g_isDragging = FALSE; 
            ReleaseCapture();
    
            ShowWindow(hwnd, SW_HIDE);
            DestroyWindow(hwnd);
            
            // Force desktop to repaint immediately to clear any ghosting artifacts
            RedrawWindow(NULL, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_ERASE);
            return 0;
        }
        
        case WM_DESTROY: {
            g_isRegionCapturing = FALSE;
            g_regionCaptureEndTime = GetTickCount(); // Start the 50ms block grace period
            
            // DO NOT call uninstall_mouse_hook() here anymore!
            if (g_hdcBlack) { DeleteDC(g_hdcBlack); g_hdcBlack = NULL; }
            if (g_hbmBlack) { DeleteObject(g_hbmBlack); g_hbmBlack = NULL; }
            if (g_hdcMem)   { DeleteDC(g_hdcMem); g_hdcMem = NULL; }
            if (g_hbmScreen){ DeleteObject(g_hbmScreen); g_hbmScreen = NULL; }
            
            // Completely clear all region selection state so the app remembers nothing
            g_hwndRegion = NULL;
            g_isDragging = FALSE;
            g_rcSel.left = g_rcSel.top = g_rcSel.right = g_rcSel.bottom = 0;
            
            if (!g_cfg.show_cursor) ShowCursor(TRUE);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void start_region_capture(void) {

    // Fallback safety: If the flag was somehow stuck, reset it before starting
    g_isRegionCapturing = TRUE; 
    g_regionCaptureEndTime = 0;

    if (g_hwndRegion) return;

    g_isRegionCapturing = TRUE;

    // Ensure a completely clean slate before starting a new capture
    g_isDragging = FALSE;
    g_rcSel.left = g_rcSel.top = g_rcSel.right = g_rcSel.bottom = 0;

    g_screenW = GetSystemMetrics(SM_CXSCREEN); g_screenH = GetSystemMetrics(SM_CYSCREEN);
    HDC g_hdcScreen = GetDC(NULL);
    g_hdcMem = CreateCompatibleDC(g_hdcScreen);
    
    BITMAPINFO bi = {0}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = g_screenW; bi.bmiHeader.biHeight = -g_screenH;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    g_hbmScreen = CreateDIBSection(g_hdcScreen, &bi, DIB_RGB_COLORS, NULL, NULL, 0);
    SelectObject(g_hdcMem, g_hbmScreen);
    BitBlt(g_hdcMem, 0, 0, g_screenW, g_screenH, g_hdcScreen, 0, 0, SRCCOPY);

    g_hdcBlack = CreateCompatibleDC(g_hdcScreen);
    g_hbmBlack = CreateCompatibleBitmap(g_hdcScreen, 1, 1);
    HBITMAP hOldBmp = (HBITMAP)SelectObject(g_hdcBlack, g_hbmBlack);
    RECT rc = {0, 0, 1, 1};
    FillRect(g_hdcBlack, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
    SelectObject(g_hdcBlack, hOldBmp);

    ReleaseDC(NULL, g_hdcScreen);


    WNDCLASSEXW wc = {0}; wc.cbSize = sizeof(wc); wc.lpfnWndProc = RegionWndProc;
    wc.hInstance = GetModuleHandle(NULL); wc.hCursor = LoadCursor(NULL, IDC_CROSS);
    wc.lpszClassName = L"JxlShotRegionClass"; RegisterClassExW(&wc);

    g_hwndRegion = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"JxlShotRegionClass", L"",
                                   WS_POPUP, 0, 0, g_screenW, g_screenH, NULL, NULL, GetModuleHandle(NULL), NULL);
    
    // If window creation fails, clean up GDI objects immediately to prevent leaks
    if (!g_hwndRegion) {
        if (g_hdcBlack) { DeleteDC(g_hdcBlack); g_hdcBlack = NULL; }
        if (g_hbmBlack) { DeleteObject(g_hbmBlack); g_hbmBlack = NULL; }
        if (g_hdcMem)   { DeleteDC(g_hdcMem); g_hdcMem = NULL; }
        if (g_hbmScreen){ DeleteObject(g_hbmScreen); g_hbmScreen = NULL; }
        return;
    }

    if (!g_cfg.show_cursor) ShowCursor(FALSE);
    
    ShowWindow(g_hwndRegion, SW_SHOW); 
    
    // Crucial Fix: Force the overlay to take keyboard focus so it receives WM_KEYDOWN (Escape)
    // We use AttachThreadInput to bypass Windows' strict SetForegroundWindow limitations
    HWND hCurWnd = GetForegroundWindow();
    DWORD dwCurID = GetCurrentThreadId();
    DWORD dwForeID = GetWindowThreadProcessId(hCurWnd, NULL);
    
    if (dwCurID != dwForeID) {
        AttachThreadInput(dwCurID, dwForeID, TRUE);
        SetForegroundWindow(g_hwndRegion);
        SetFocus(g_hwndRegion);
        AttachThreadInput(dwCurID, dwForeID, FALSE);
    } else {
        SetForegroundWindow(g_hwndRegion);
        SetFocus(g_hwndRegion);
    }
    
    UpdateWindow(g_hwndRegion);
    install_mouse_hook();
}



// Low-Level Keyboard Hook


/* Helper function to verify EXACT modifier match. 
 * If the INI requires Ctrl, Ctrl must be pressed. 
 * If the INI does NOT require Ctrl, Ctrl must NOT be pressed.
 */

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
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT *pKB = (KBDLLHOOKSTRUCT *)lParam;
        
        // FIX: Intercept Escape globally while region selection is active.
        // This guarantees Esc works even if Windows refuses to give the overlay keyboard focus,
        // and prevents the Esc key from accidentally exiting fullscreen games or closing underlying menus.
        if (g_hwndRegion && pKB->vkCode == VK_ESCAPE && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
            PostMessageW(g_hwndRegion, WM_KEYDOWN, VK_ESCAPE, 0);
            return 1; // Block the key from reaching the underlying application
        }

        if (wParam == WM_KEYDOWN) {
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
    }
    
    // If no match, or if the configured VK is 0 (null/disabled), pass the key to the OS normally
    return CallNextHookEx(g_hhkKeyboard, nCode, wParam, lParam);
}

static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        // Block if currently capturing, OR within 25ms after capturing ended (catches the mouse release)
        BOOL is_active = g_isRegionCapturing || (GetTickCount() - g_regionCaptureEndTime < 500);
        
        if (is_active && (wParam == WM_RBUTTONDOWN || wParam == WM_RBUTTONUP)) {
            if (wParam == WM_RBUTTONDOWN && g_hwndRegion) {
                PostMessageW(g_hwndRegion, WM_RBUTTONDOWN, 0, 0);
            }
            return 1; // Completely block the event from reaching the OS / underlying apps
        }
    }
    return CallNextHookEx(g_hhkMouse, nCode, wParam, lParam);
}

static void install_keyboard_hook(void) {
    if (g_hhkKeyboard) return;
    g_hhkKeyboard = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandle(NULL), 0);
}

static void uninstall_keyboard_hook(void) {
    if (g_hhkKeyboard) { UnhookWindowsHookEx(g_hhkKeyboard); g_hhkKeyboard = NULL; }
}

static void install_mouse_hook(void) {
    if (g_hhkMouse) return;
    g_hhkMouse = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, GetModuleHandle(NULL), 0);
}

static void uninstall_mouse_hook(void) {
    if (g_hhkMouse) { 
        UnhookWindowsHookEx(g_hhkMouse); 
        g_hhkMouse = NULL; 
    }
}



// Tray Window Procedure & Entry Point



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
                case IDM_OPENEXPORT: execute_open_export_folder(); break;
                case IDM_OPENCONFIG: execute_open_config(hwnd); break;
                case IDM_RELOAD: reload_config(); break;
                case IDM_CHECK_UPDATE: execute_check_update(hwnd); break;
                case IDM_ABOUT: execute_about(); break;
                case IDM_EXIT: PostQuitMessage(0); break;
            } break;
        case WM_DESTROY:
            uninstall_keyboard_hook();
            uninstall_mouse_hook(); // clean up when the app fully exits
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
    // 1. Initialize COM for Shell APIs (SHBrowseForFolder), TaskDialog, and DXGI/D3D11 stability
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    set_dpi_aware(); 
    init_paths(); 
    ensure_default_ini(); 
    init_config();
    dbg_init();
    // Initialize dark mode BEFORE creating any windows or dialogs
    InitializeDarkMode();

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
    
    // Clean up uxtheme before exiting
    if (g_hUxtheme) {
        FreeLibrary(g_hUxtheme);
        g_hUxtheme = NULL;
    }

    // Clean up COM before exiting
    CoUninitialize();
    
    return (int)msg.wParam;
}