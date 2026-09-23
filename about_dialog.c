// about_dialog.c

// Adobe-Style Frameless Splash About Dialog

#include <windows.h>
#include <windowsx.h> // For GET_X_LPARAM / GET_Y_LPARAM
#include <commctrl.h>
#include <shellapi.h>

#define IDC_ABOUT_TITLE 1001
#define IDC_ABOUT_DESCRIPTION 1002
#define IDC_ABOUT_LINK 1003
#define IDC_ABOUT_ICON 1005

static HBRUSH g_hAboutBgBrush = NULL;
static HFONT g_hTitleFont = NULL;
static HFONT g_hBodyFont = NULL;
static HFONT g_hXFont = NULL;
static HICON g_hAppIcon = NULL;
static BOOL g_isXHovered = FALSE;

// Forward declaration (assumed to be defined elsewhere in your codebase)
extern void ApplyDarkMode(HWND hwnd);
extern HWND g_hwndTray;
extern HWND g_hwndMenuOwner;

static LRESULT CALLBACK AboutWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_CREATE: {
            ApplyDarkMode(hwnd);

            // 1. Enable Windows 11 Rounded Corners dynamically
            HMODULE hDwmapi = LoadLibraryW(L"dwmapi.dll");
            if (hDwmapi) {
                typedef HRESULT (WINAPI *pDwmSetWindowAttribute)(HWND, DWORD, LPCVOID, DWORD);
                pDwmSetWindowAttribute pDwmSet = (pDwmSetWindowAttribute)GetProcAddress(hDwmapi, "DwmSetWindowAttribute");
                if (pDwmSet) {
                    int preference = 2; // DWMWCP_ROUND
                    pDwmSet(hwnd, 33, &preference, sizeof(preference)); // 33 = DWMWA_WINDOW_CORNER_PREFERENCE
                }
                FreeLibrary(hDwmapi);
            }

            // 2. Create Modern Typography
            g_hTitleFont = CreateFontW(-24, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Semibold");
            g_hBodyFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            g_hXFont = CreateFontW(-20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

            // 3. Create Background Brush (Modern Dark Gray #1E1E1E)
            g_hAboutBgBrush = CreateSolidBrush(RGB(30, 30, 30));

            // 4. Load App Icon (128x128 for large splash display, preserving original aspect)
            g_hAppIcon = (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 128, 128, 0);
            
            // Set the window's own icon (shows in Taskbar / Alt-Tab)
            SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)g_hAppIcon);
            SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)g_hAppIcon);

            // 5. Layout Controls
            
            // Title (Left aligned)
            HWND hTitle = CreateWindowExW(0, L"STATIC", L"JXL Screenshot Tool", 
                WS_CHILD | WS_VISIBLE | SS_LEFT, 30, 40, 250, 30, hwnd, (HMENU)IDC_ABOUT_TITLE, GetModuleHandleW(NULL), NULL);
            SendMessageW(hTitle, WM_SETFONT, (WPARAM)g_hTitleFont, TRUE);

            // Description (Left aligned)
            HWND hDesc = CreateWindowExW(0, L"STATIC", L"Minimal tray screenshot tool\nusing JPEG XL.", 
                WS_CHILD | WS_VISIBLE | SS_LEFT, 30, 80, 250, 50, hwnd, (HMENU)IDC_ABOUT_DESCRIPTION, GetModuleHandleW(NULL), NULL);
            SendMessageW(hDesc, WM_SETFONT, (WPARAM)g_hBodyFont, TRUE);

            // Hyperlink (Left aligned)
            CreateWindowExW(0, WC_LINK, L"<a href=\"https://github.com/azizr12/jxlscrenshot\">View on GitHub</a>", 
                WS_CHILD | WS_VISIBLE | LWS_TRANSPARENT, 30, 140, 200, 20, hwnd, (HMENU)IDC_ABOUT_LINK, GetModuleHandleW(NULL), NULL);
            
            // Splash Icon (Right aligned, large 128x128)
            HWND hIconCtrl = CreateWindowExW(0, L"STATIC", L"", 
                WS_CHILD | WS_VISIBLE | SS_ICON | SS_CENTERIMAGE, 320, 30, 128, 128, hwnd, (HMENU)IDC_ABOUT_ICON, GetModuleHandleW(NULL), NULL);
            SendMessageW(hIconCtrl, STM_SETICON, (WPARAM)g_hAppIcon, 0);

            return 0;
        }

        case WM_ERASEBKGND: {
            // Paint background immediately to prevent white flashing
            HDC hdc = (HDC)wParam;
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, g_hAboutBgBrush);
            return 1; 
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            
            // Fill Background
            FillRect(hdc, &rc, g_hAboutBgBrush);
            
            // Draw Custom '✕' Exit Button (Top Right) in RED
            SetBkMode(hdc, TRANSPARENT);
            
            RECT rcX = {440, 10, 480, 50}; // 40x40 clickable area aligned to right edge
            
            if (g_isXHovered) {
                // Hover state: darker red background, brighter red text
                HBRUSH hHoverBrush = CreateSolidBrush(RGB(60, 30, 30));
                FillRect(hdc, &rcX, hHoverBrush);
                DeleteObject(hHoverBrush);
                SetTextColor(hdc, RGB(255, 120, 120));
            } else {
                // Normal state: transparent background, modern red text
                SetTextColor(hdc, RGB(255, 85, 85));
            }
            
            HFONT hOldFont = (HFONT)SelectObject(hdc, g_hXFont);
            DrawTextW(hdc, L"\u2715", -1, &rcX, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(hdc, hOldFont);
            
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_CTLCOLORSTATIC: {
            HDC hdcStatic = (HDC)wParam;
            HWND hCtrl = (HWND)lParam;
            int id = GetDlgCtrlID(hCtrl);
            
            SetBkMode(hdcStatic, TRANSPARENT);
            
            if (id == IDC_ABOUT_TITLE) {
                SetTextColor(hdcStatic, RGB(255, 255, 255)); // White
            } else if (id == IDC_ABOUT_DESCRIPTION) {
                SetTextColor(hdcStatic, RGB(160, 160, 160)); // Light Gray
            }
            
            return (INT_PTR)g_hAboutBgBrush;
        }

        case WM_MOUSEMOVE: {
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            RECT rcX = {440, 10, 480, 50};
            POINT pt = {x, y};
            BOOL isHovered = PtInRect(&rcX, pt);
            
            if (isHovered != g_isXHovered) {
                g_isXHovered = isHovered;
                InvalidateRect(hwnd, &rcX, TRUE);
                
                if (isHovered) {
                    TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
                    TrackMouseEvent(&tme);
                    SetCursor(LoadCursorW(NULL, IDC_HAND));
                } else {
                    SetCursor(LoadCursorW(NULL, IDC_ARROW));
                }
            } else if (isHovered) {
                SetCursor(LoadCursorW(NULL, IDC_HAND));
            }
            return 0;
        }

        case WM_MOUSELEAVE: {
            g_isXHovered = FALSE;
            RECT rcX = {440, 10, 480, 50};
            InvalidateRect(hwnd, &rcX, TRUE);
            SetCursor(LoadCursorW(NULL, IDC_ARROW));
            return 0;
        }

        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;

        case WM_LBUTTONDOWN: {
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            
            // Hit-test for custom Red '✕' Exit Button
            if (x >= 440 && x <= 480 && y >= 10 && y <= 50) {
                DestroyWindow(hwnd);
                return 0;
            }
            
            // Allow dragging the window by clicking anywhere on the background
            HWND hChild = ChildWindowFromPoint(hwnd, (POINT){x, y});
            if (hChild == hwnd) {
                ReleaseCapture();
                SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, MAKELPARAM(x, y));
            }
            return 0;
        }

        case WM_NOTIFY: {
            LPNMHDR pnmh = (LPNMHDR)lParam;
            if (pnmh->idFrom == IDC_ABOUT_LINK && pnmh->code == NM_CLICK) {
                NMLINK* pnmLink = (NMLINK*)lParam;
                ShellExecuteW(hwnd, L"open", pnmLink->item.szUrl, NULL, NULL, SW_SHOWNORMAL);
                return 0;
            }
            break;
        }

        case WM_ACTIVATE:
            // Keep the splash screen on top if it loses focus
            if (wParam == WA_INACTIVE) {
                SetForegroundWindow(hwnd);
            }
            break;

        case WM_DESTROY:
            EnableWindow(g_hwndTray, TRUE);
            if (g_hAboutBgBrush) { DeleteObject(g_hAboutBgBrush); g_hAboutBgBrush = NULL; }
            if (g_hTitleFont) { DeleteObject(g_hTitleFont); g_hTitleFont = NULL; }
            if (g_hBodyFont) { DeleteObject(g_hBodyFont); g_hBodyFont = NULL; }
            if (g_hXFont) { DeleteObject(g_hXFont); g_hXFont = NULL; }
            if (g_hAppIcon) { DestroyIcon(g_hAppIcon); g_hAppIcon = NULL; }
            PostQuitMessage(0); 
            return 0;

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

static void execute_about(void) {
    // Disable the tray window while the dialog is open (modal behavior)
    EnableWindow(g_hwndTray, FALSE);
    
    // Grant this process the right to set its own windows to the foreground
    AllowSetForegroundWindow(GetCurrentProcessId());

    // Ensure common controls (specifically SysLink) are initialized
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LINK_CLASS };
    InitCommonControlsEx(&icc);

    // Dimensions adjusted to comfortably accommodate the larger 128x128 icon
    const int dlgWidth = 480;
    const int dlgHeight = 240;

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = AboutWindowProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL; 
    wc.lpszClassName = L"JxlShotAboutClass";
    
    // Assign the window its own icon for Taskbar and Alt-Tab visibility
    wc.hIcon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hIconSm = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_APP_ICON));
    
    RegisterClassExW(&wc);

    // Center the window on the primary monitor work area
    RECT rc;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &rc, 0);
    int x = rc.left + (rc.right - rc.left - dlgWidth) / 2;
    int y = rc.top + (rc.bottom - rc.top - dlgHeight) / 2;

    // Create Borderless Window (WS_POPUP)
    HWND hwndAbout = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_APPWINDOW,
        L"JxlShotAboutClass",
        L"About",
        WS_POPUP | WS_VISIBLE, // No WS_CAPTION or WS_SYSMENU
        x, y, dlgWidth, dlgHeight,
        g_hwndMenuOwner,
        NULL,
        GetModuleHandleW(NULL),
        NULL
    );

    if (hwndAbout) {
        SetForegroundWindow(hwndAbout);

        // Local modal message loop
        MSG msg;
        while (GetMessage(&msg, NULL, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    } else {
        EnableWindow(g_hwndTray, TRUE);
        MessageBoxW(NULL, L"Failed to create About dialog.", L"jxlshot", MB_ICONERROR);
    }
}