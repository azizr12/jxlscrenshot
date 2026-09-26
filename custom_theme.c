// custom_theme.c
// Custom dark-themed UI components (Menu, About Dialog)

#define UNICODE
#define _UNICODE
#define WINVER 0x0601
#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include "resource.h"

// Forward declaration for core tray state
extern HWND g_hwndTray;

#define MENU_ITEM_COUNT 11

typedef struct {
    UINT id;
    const wchar_t* text;
    BOOL is_separator;
} MenuItem;

static const MenuItem g_menuItems[] = {
    { IDM_FULL, L"Capture Full Screen", FALSE },
    { IDM_REGION, L"Capture Region...", FALSE },
    { 0, NULL, TRUE },
    { IDM_SETPATH, L"Set Export Path...", FALSE },
    { IDM_OPENEXPORT, L"Open Export Folder", FALSE },
    { IDM_OPENCONFIG, L"Open Config File", FALSE },
    { IDM_RELOAD, L"Reload Configuration", FALSE },
    { IDM_CHECK_UPDATE, L"Check for Updates...", FALSE },
    { 0, NULL, TRUE },
    { IDM_ABOUT, L"About...", FALSE },
    { IDM_EXIT, L"Exit", FALSE }
};

static HWND g_hwndMenu = NULL;

static LRESULT CALLBACK MenuWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_CREATE: {
            HMODULE hDwmapi = LoadLibraryW(L"dwmapi.dll");
            if (hDwmapi) {
                typedef HRESULT (WINAPI *pDwmSetWindowAttribute)(HWND, DWORD, LPCVOID, DWORD);
                pDwmSetWindowAttribute pDwmSet = (pDwmSetWindowAttribute)GetProcAddress(hDwmapi, "DwmSetWindowAttribute");
                if (pDwmSet) {
                    int preference = 2; // DWMWCP_ROUND
                    pDwmSet(hwnd, 33, &preference, sizeof(preference));
                }
                FreeLibrary(hDwmapi);
            }
            
            static HFONT hMenuFont = NULL;
            if (!hMenuFont) {
                hMenuFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 
                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            }
            SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)hMenuFont);
            
            static HBRUSH hMenuBgBrush = NULL;
            if (!hMenuBgBrush) {
                hMenuBgBrush = CreateSolidBrush(RGB(30, 30, 30));
            }
            SetWindowLongPtr(hwnd, GWLP_USERDATA + sizeof(LONG_PTR), (LONG_PTR)hMenuBgBrush);
            SetWindowLongPtr(hwnd, GWLP_USERDATA + sizeof(LONG_PTR) * 2, 0); // hovered_id
            
            return 0;
        }
        case WM_ERASEBKGND: {
            HDC hdc = (HDC)wParam;
            RECT rc;
            GetClientRect(hwnd, &rc);
            HBRUSH hBrush = (HBRUSH)GetWindowLongPtr(hwnd, GWLP_USERDATA + sizeof(LONG_PTR));
            FillRect(hdc, &rc, hBrush);
            return 1;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            
            HBRUSH hBrush = (HBRUSH)GetWindowLongPtr(hwnd, GWLP_USERDATA + sizeof(LONG_PTR));
            FillRect(hdc, &rc, hBrush);
            
            SetBkMode(hdc, TRANSPARENT);
            HFONT hFont = (HFONT)GetWindowLongPtr(hwnd, GWLP_USERDATA);
            HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);
            
            int hovered_id = (int)(INT_PTR)GetWindowLongPtr(hwnd, GWLP_USERDATA + sizeof(LONG_PTR) * 2);
            int current_y = 8;
            
            for (int i = 0; i < MENU_ITEM_COUNT; i++) {
                if (g_menuItems[i].is_separator) {
                    HPEN hPen = CreatePen(PS_SOLID, 1, RGB(68, 68, 68));
                    HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
                    MoveToEx(hdc, 12, current_y + 4, NULL);
                    LineTo(hdc, 228, current_y + 4);
                    SelectObject(hdc, hOldPen);
                    DeleteObject(hPen);
                    current_y += 10;
                } else {
                    RECT item_rc = { 0, current_y, 240, current_y + 32 };
                    if (g_menuItems[i].id == (UINT)hovered_id) {
                        HBRUSH hHoverBrush = CreateSolidBrush(RGB(51, 51, 51));
                        FillRect(hdc, &item_rc, hHoverBrush);
                        DeleteObject(hHoverBrush);
                    }
                    
                    SetTextColor(hdc, RGB(255, 255, 255));
                    RECT text_rc = { 24, current_y, 232, current_y + 32 };
                    DrawTextW(hdc, g_menuItems[i].text, -1, &text_rc, DT_VCENTER | DT_SINGLELINE | DT_LEFT);
                    
                    current_y += 32;
                }
            }
            
            SelectObject(hdc, hOldFont);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_MOUSEMOVE: {
            int y = GET_Y_LPARAM(lParam);
            int current_y = 8;
            int new_hovered_id = 0;
            
            for (int i = 0; i < MENU_ITEM_COUNT; i++) {
                if (g_menuItems[i].is_separator) {
                    current_y += 10;
                } else {
                    if (y >= current_y && y < current_y + 32) {
                        new_hovered_id = (int)g_menuItems[i].id;
                        break;
                    }
                    current_y += 32;
                }
            }
            
            int old_hovered_id = (int)(INT_PTR)GetWindowLongPtr(hwnd, GWLP_USERDATA + sizeof(LONG_PTR) * 2);
            if (new_hovered_id != old_hovered_id) {
                SetWindowLongPtr(hwnd, GWLP_USERDATA + sizeof(LONG_PTR) * 2, (LONG_PTR)new_hovered_id);
                InvalidateRect(hwnd, NULL, FALSE);
                
                if (new_hovered_id != 0) {
                    TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
                    TrackMouseEvent(&tme);
                    SetCursor(LoadCursorW(NULL, IDC_HAND));
                } else {
                    SetCursor(LoadCursorW(NULL, IDC_ARROW));
                }
            }
            return 0;
        }
        case WM_MOUSELEAVE: {
            SetWindowLongPtr(hwnd, GWLP_USERDATA + sizeof(LONG_PTR) * 2, 0);
            InvalidateRect(hwnd, NULL, FALSE);
            SetCursor(LoadCursorW(NULL, IDC_ARROW));
            return 0;
        }
        case WM_LBUTTONDOWN: {
            int y = GET_Y_LPARAM(lParam);
            int current_y = 8;
            int clicked_id = 0;
            
            for (int i = 0; i < MENU_ITEM_COUNT; i++) {
                if (g_menuItems[i].is_separator) {
                    current_y += 10;
                } else {
                    if (y >= current_y && y < current_y + 32) {
                        clicked_id = (int)g_menuItems[i].id;
                        break;
                    }
                    current_y += 32;
                }
            }
            
            if (clicked_id != 0) {
                PostMessage(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(clicked_id, 0), 0);
            }
            ReleaseCapture();
            DestroyWindow(hwnd);
            return 0;
        }
        case WM_KILLFOCUS:
        case WM_CANCELMODE: {
            ReleaseCapture();
            DestroyWindow(hwnd);
            return 0;
        }
        case WM_KEYDOWN: {
            if (wParam == VK_ESCAPE) {
                ReleaseCapture();
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        }
        case WM_DESTROY: {
            g_hwndMenu = NULL;
            return 0;
        }
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

void init_custom_ui(HINSTANCE hInst) {
    WNDCLASSEXW wcMenu = {0}; 
    wcMenu.cbSize = sizeof(wcMenu); 
    wcMenu.lpfnWndProc = MenuWindowProc;
    wcMenu.hInstance = hInst; 
    wcMenu.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wcMenu.lpszClassName = L"JxlShotMenuClass"; 
    RegisterClassExW(&wcMenu);
}

void show_tray_menu(HWND hwnd) {
    POINT pt; 
    GetCursorPos(&pt);
    
    int menuWidth = 240;
    int menuHeight = 280;
    
    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    if (GetMonitorInfoW(hMon, &mi)) {
        if (pt.x + menuWidth > mi.rcWork.right) {
            pt.x = mi.rcWork.right - menuWidth;
        }
        if (pt.y + menuHeight > mi.rcWork.bottom) {
            pt.y = mi.rcWork.bottom - menuHeight;
        }
    }
    
    g_hwndMenu = CreateWindowExW(
        WS_EX_TOPMOST,
        L"JxlShotMenuClass",
        L"",
        WS_POPUP | WS_VISIBLE,
        pt.x, pt.y, menuWidth, menuHeight,
        hwnd, NULL, GetModuleHandleW(NULL), NULL
    );
    
    if (g_hwndMenu) {
        SetForegroundWindow(g_hwndMenu);
        SetCapture(g_hwndMenu);
    }
}

// Include the fully restored About Dialog implementation
#include "about_dialog.c"