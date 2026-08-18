/* jxlshot_tray.c — System tray extension for jxlshot.
 *
 * Configuration is read from jxlshot.ini located next to the executable.
 * Debug logs are written to %TEMP%\jxlshot_debug.log.
 *
 *
 * Build (MSYS2 / MinGW-w64) - Optimized for size:
 *   gcc -Os -s -flto -ffunction-sections -fdata-sections -Wl,--gc-sections \
 *       -mwindows -o jxlshot_tray.exe jxlshot_tray.c -ljxl -lgdi32 -luser32 -lshell32 -lcomctl32 -lmsimg32 -lole32
 */
#define UNICODE
#define _UNICODE
#define WINVER 0x0601
#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN
#define JXLSHOT_TRAY_BUILD

#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <windowsx.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include "jxlshot.c" // Pulls in core logic, config, and unified dbg() logger

#define WM_TRAYICON            (WM_USER + 1)
#define WM_HOOK_FULL_CAPTURE   (WM_USER + 10)
#define WM_HOOK_REGION_CAPTURE (WM_USER + 11)
#define ID_TRAY       1
#define IDM_FULL      101
#define IDM_REGION    102
#define IDM_EXIT      103
#define IDM_SETPATH   104
#define IDM_ABOUT     105

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
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, IDM_ABOUT, L"About...");
    AppendMenuW(hMenu, MF_STRING, IDM_EXIT, L"Exit");
    SetForegroundWindow(hwnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(hMenu); PostMessage(hwnd, WM_NULL, 0, 0);
}

static void execute_full_capture(void) {
    reload_config(); // FIX: Ensure config is fresh before capturing
    Grab g;
    if (!grab_primary_monitor(&g)) {
        free_grab(&g); MessageBoxW(NULL, L"Screen capture failed.", L"jxlshot", MB_ICONERROR); return;
    }
    wchar_t out_path[MAX_PATH]; build_out_path(out_path, MAX_PATH);
    if (!save_bgra_as_jxl(g.bits, g.w, g.h, g_cfg.lossless, g_cfg.distance, out_path)) {
        MessageBoxW(NULL, L"Encoding or saving failed.", L"jxlshot", MB_ICONERROR);
    }
    free_grab(&g);
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

/* ------------------------------------------------------------------ */
/* About Dialog with Clickable Hyperlink                              */
/* ------------------------------------------------------------------ */
static HRESULT CALLBACK AboutDialogCallback(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, LONG_PTR lpRefData) {
    if (msg == TDN_HYPERLINK_CLICKED) {
        // lParam contains the URL string extracted from the <a href="..."> tag
        ShellExecuteW(hwnd, L"open", (LPCWSTR)lParam, NULL, NULL, SW_SHOWNORMAL);
    }
    return S_OK;
}

static void execute_about(void) {
    TASKDIALOGCONFIG config = {0};
    config.cbSize = sizeof(TASKDIALOGCONFIG);
    config.hwndParent = NULL;
    config.hInstance = NULL;
    config.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_ALLOW_DIALOG_CANCELLATION;
    config.pszWindowTitle = L"About";
    config.pszMainIcon = TD_INFORMATION_ICON;
    config.pszMainInstruction = L"JXL Screenshot Tool";
    config.pszContent = L"Minimal tray screenshot tool using JPEG XL.\n\n"
                        L"<a href=\"https://github.com/azizr12/jxlscrenshot\">https://github.com/azizr12/jxlscrenshot</a>";
    config.pfCallback = AboutDialogCallback;

    TaskDialogIndirect(&config, NULL, NULL, NULL);
}


/*       https://github.com/azizr12/jxlscrenshot/        */


/* ------------------------------------------------------------------ */
/* Interactive Region Selection (Optimized & Ghosting Fixed)          */
/* ------------------------------------------------------------------ */
static HWND   g_hwndRegion = NULL;
static HDC    g_hdcMem = NULL, g_hdcBlack = NULL;
static HBITMAP g_hbmScreen = NULL, g_hbmBlack = NULL;
static int    g_screenW, g_screenH;
static RECT   g_rcSel;
static BOOL   g_isDragging = FALSE;

static void crop_and_encode_region(RECT *r) {
    Grab g;
    if (!grab_primary_monitor(&g)) { free_grab(&g); return; }
    int rw = r->right - r->left, rh = r->bottom - r->top;
    if (rw <= 0 || rh <= 0) { free_grab(&g); return; }

    uint8_t *crop_bits = (uint8_t *)malloc((size_t)rw * rh * 4);
    if (!crop_bits) { free_grab(&g); return; }
    for (int y = 0; y < rh; y++) {
        memcpy(crop_bits + y * rw * 4, g.bits + ((r->top + y) * g.w + r->left) * 4, rw * 4);
    }
    wchar_t out_path[MAX_PATH]; build_out_path(out_path, MAX_PATH);
    save_bgra_as_jxl(crop_bits, rw, rh, g_cfg.lossless, g_cfg.distance, out_path);
    free(crop_bits); free_grab(&g);
}

LRESULT CALLBACK RegionWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
            BitBlt(hdc, 0, 0, g_screenW, g_screenH, g_hdcMem, 0, 0, SRCCOPY);
            
            // OPTIMIZATION: AlphaBlend automatically stretches the 1x1 source bitmap
            BLENDFUNCTION bf = { AC_SRC_OVER, 0, 120, 0 };
            AlphaBlend(hdc, 0, 0, g_screenW, g_screenH, g_hdcBlack, 0, 0, 1, 1, bf);

            if (g_isDragging || (g_rcSel.right > g_rcSel.left && g_rcSel.bottom > g_rcSel.top)) {
                int x = min(g_rcSel.left, g_rcSel.right), y = min(g_rcSel.top, g_rcSel.bottom);
                int w = abs(g_rcSel.right - g_rcSel.left), h = abs(g_rcSel.bottom - g_rcSel.top);
                BitBlt(hdc, x, y, w, h, g_hdcMem, x, y, SRCCOPY);
                HPEN hPen = CreatePen(PS_SOLID, 2, RGB(0, 120, 215));
                HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
                HBRUSH hOldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
                Rectangle(hdc, x, y, x + w, y + h);
                SelectObject(hdc, hOldBrush); SelectObject(hdc, hOldPen); DeleteObject(hPen);
            }
            EndPaint(hwnd, &ps); return 0;
        }
        case WM_LBUTTONDOWN: {
            g_isDragging = TRUE; g_rcSel.left = GET_X_LPARAM(lp); g_rcSel.top = GET_Y_LPARAM(lp);
            g_rcSel.right = g_rcSel.left; g_rcSel.bottom = g_rcSel.top;
            SetCapture(hwnd); InvalidateRect(hwnd, NULL, FALSE); return 0;
        }
        case WM_MOUSEMOVE: {
            if (g_isDragging) {
                g_rcSel.right = GET_X_LPARAM(lp); g_rcSel.bottom = GET_Y_LPARAM(lp);
                InvalidateRect(hwnd, NULL, FALSE);
            } return 0;
        }
        case WM_LBUTTONUP: {
            if (g_isDragging) {
            g_isDragging = FALSE; 
            ReleaseCapture();
        
            RECT r = g_rcSel;
            if (r.left > r.right) { int t = r.left; r.left = r.right; r.right = t; }
            if (r.top > r.bottom) { int t = r.top; r.top = r.bottom; r.bottom = t; }
        
            // 1. Remove the window from DWM composition immediately
            ShowWindow(hwnd, SW_HIDE);
            DestroyWindow(hwnd);
        
            // 2. Force a synchronous, immediate repaint of the entire desktop
            RedrawWindow(NULL, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        
            // 3. Yield execution briefly to allow DWM to composite the clean frame
            Sleep(50); 

            // 4. Capture the screen only after the buffer is guaranteed to be clean
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
        
                // 1. Remove the overlay window from DWM composition immediately
                ShowWindow(hwnd, SW_HIDE);
                DestroyWindow(hwnd);
        
                // 2. Force a synchronous, immediate repaint of the entire desktop
                RedrawWindow(NULL, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        
                // 3. Yield execution briefly to allow DWM to composite the clean frame
                Sleep(50); 
            } 
            return 0;
        }
        case WM_DESTROY: {
            if (g_hdcBlack) { DeleteDC(g_hdcBlack); g_hdcBlack = NULL; }
            if (g_hbmBlack) { DeleteObject(g_hbmBlack); g_hbmBlack = NULL; }
            if (g_hdcMem)   { DeleteDC(g_hdcMem); g_hdcMem = NULL; }
            if (g_hbmScreen){ DeleteObject(g_hbmScreen); g_hbmScreen = NULL; }
            g_hwndRegion = NULL;
            if (!g_cfg.show_cursor) ShowCursor(TRUE);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void start_region_capture(void) {
    if (g_hwndRegion) return;
    reload_config(); // FIX: Ensure config is fresh before capturing

    g_screenW = GetSystemMetrics(SM_CXSCREEN); g_screenH = GetSystemMetrics(SM_CYSCREEN);
    HDC g_hdcScreen = GetDC(NULL);
    g_hdcMem = CreateCompatibleDC(g_hdcScreen);
    
    BITMAPINFO bi = {0}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = g_screenW; bi.bmiHeader.biHeight = -g_screenH;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    g_hbmScreen = CreateDIBSection(g_hdcScreen, &bi, DIB_RGB_COLORS, NULL, NULL, 0);
    SelectObject(g_hdcMem, g_hbmScreen);
    BitBlt(g_hdcMem, 0, 0, g_screenW, g_screenH, g_hdcScreen, 0, 0, SRCCOPY);

    // OPTIMIZATION: Create a 1x1 black bitmap instead of a full-screen one to save ~33MB RAM
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
    if (!g_cfg.show_cursor) ShowCursor(FALSE);
    ShowWindow(g_hwndRegion, SW_SHOW); UpdateWindow(g_hwndRegion);
}

/* ------------------------------------------------------------------ */
/* Low-Level Keyboard Hook                                            */
/* ------------------------------------------------------------------ */

/* ofc this will get it flag as a virus
 * because its a c code and need low level access to work
 * there is no malware or monkey
 * 
 * leave the app if you are paranoid ! !
 * trying to implement something will make this just garbage app
 * that take megabytes of binary data for a stupid key detection
 * fuck false postive and fuck modern antiviruses
 * RegisterHotKey  is shit tried it and always fail
 */
static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && wParam == WM_KEYDOWN) {
        KBDLLHOOKSTRUCT *pKB = (KBDLLHOOKSTRUCT *)lParam;
        if (pKB->vkCode == VK_SNAPSHOT) {
            if (GetAsyncKeyState(VK_CONTROL) & 0x8000) PostMessageW(g_hwndTray, WM_HOOK_REGION_CAPTURE, 0, 0);
            else PostMessageW(g_hwndTray, WM_HOOK_FULL_CAPTURE, 0, 0);
            return 1; // Block key
        }
    }
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
                case IDM_ABOUT: execute_about(); break;
                case IDM_EXIT: PostQuitMessage(0); break;
            } break;
        case WM_DESTROY:
            uninstall_keyboard_hook();
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            PostQuitMessage(0); break;
        default: return DefWindowProcW(hwnd, msg, wp, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR szCmdLine, int sw) {
    set_dpi_aware(); init_paths(); ensure_default_ini(); init_config();
    dbg_init(); // Initialize the unified logger

    WNDCLASSEXW wc = {0}; wc.cbSize = sizeof(wc); wc.lpfnWndProc = TrayWndProc;
    wc.hInstance = hInst; wc.lpszClassName = L"JxlShotTrayClass"; RegisterClassExW(&wc);
    
    g_hwndTray = CreateWindowExW(0, L"JxlShotTrayClass", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hInst, NULL);
    
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid); g_nid.hWnd = g_hwndTray; g_nid.uID = ID_TRAY;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP; g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIcon(NULL, IDI_INFORMATION); wcscpy(g_nid.szTip, L"JXL Screenshot Tool");
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    install_keyboard_hook();

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }
    uninstall_keyboard_hook();
    return (int)msg.wParam;
}

/* its just a vibe code bullshit
 * there is no malware or monkey
 * leave the app if you are paranoid
 */


