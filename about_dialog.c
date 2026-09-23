
// about_dialog.c


#include <windows.h>
#include <windowsx.h> // For GET_X_LPARAM / GET_Y_LPARAM
#include <commctrl.h>
#include <shellapi.h>

#define IDC_ABOUT_TITLE 1001
#define IDC_ABOUT_DESCRIPTION 1002
#define IDC_ABOUT_LINK 1003
#define IDC_ABOUT_OK 1004
#define IDC_ABOUT_ICON 1005

static HBRUSH g_hAboutBgBrush = NULL;
static HFONT g_hTitleFont = NULL;
static HFONT g_hBodyFont = NULL;

static LRESULT CALLBACK AboutWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_CREATE: {
            ApplyDarkMode(hwnd);

            // 1. Enable Windows 11 Rounded Corners & Native Drop Shadow dynamically
            // (Loaded dynamically to prevent linker errors in CI/CD pipelines)
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
            g_hTitleFont = CreateFontW(-24, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            g_hBodyFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

            // 3. Create Background Brush (Modern Dark Gray #1E1E1E)
            g_hAboutBgBrush = CreateSolidBrush(RGB(30, 30, 30));

            // 4. Layout Controls
            // Title (Left aligned)
            HWND hTitle = CreateWindowExW(0, L"STATIC", L"JXL Screenshot Tool", 
                WS_CHILD | WS_VISIBLE | SS_LEFT, 30, 40, 260, 30, hwnd, (HMENU)IDC_ABOUT_TITLE, GetModuleHandleW(NULL), NULL);
            SendMessageW(hTitle, WM_SETFONT, (WPARAM)g_hTitleFont, TRUE);

            // Description (Left aligned)
            HWND hDesc = CreateWindowExW(0, L"STATIC", L"Minimal tray screenshot tool\nusing JPEG XL.", 
                WS_CHILD | WS_VISIBLE | SS_LEFT, 30, 85, 260, 50, hwnd, (HMENU)IDC_ABOUT_DESCRIPTION, GetModuleHandleW(NULL), NULL);
            SendMessageW(hDesc, WM_SETFONT, (WPARAM)g_hBodyFont, TRUE);

            // Hyperlink (Left aligned)
            CreateWindowExW(0, WC_LINK, L"<a href=\"https://github.com/azizr12/jxlscrenshot\">View on GitHub</a>", 
                WS_CHILD | WS_VISIBLE | LWS_TRANSPARENT, 30, 145, 150, 20, hwnd, (HMENU)IDC_ABOUT_LINK, GetModuleHandleW(NULL), NULL);

            // Splash Icon (Right aligned, large)
            HWND hIconCtrl = CreateWindowExW(0, L"STATIC", L"", 
                WS_CHILD | WS_VISIBLE | SS_ICON | SS_CENTERIMAGE, 320, 40, 96, 96, hwnd, (HMENU)IDC_ABOUT_ICON, GetModuleHandleW(NULL), NULL);
            HICON hIcon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_APP_ICON));
            SendMessageW(hIconCtrl, STM_SETICON, (WPARAM)hIcon, 0);

            // OK Button (Bottom Right)
            CreateWindowExW(0, L"BUTTON", L"OK", 
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP, 320, 160, 100, 32, hwnd, (HMENU)IDC_ABOUT_OK, GetModuleHandleW(NULL), NULL);

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
            
            // Draw Custom '✕' Exit Button (Top Right)
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(160, 160, 160));
            HFONT hXFont = CreateFontW(-20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            HFONT hOldFont = (HFONT)SelectObject(hdc, hXFont);
            
            RECT rcX = {410, 10, 440, 40};
            DrawTextW(hdc, L"✕", -1, &rcX, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            
            SelectObject(hdc, hOldFont);
            DeleteObject(hXFont);
            
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

        case WM_LBUTTONDOWN: {
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            
            // Hit-test for custom '✕' Exit Button
            if (x >= 410 && x <= 440 && y >= 10 && y <= 40) {
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

        case WM_COMMAND:
            if (LOWORD(wParam) == IDC_ABOUT_OK || LOWORD(wParam) == IDCANCEL) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;

        case WM_NOTIFY: {
            LPNMHDR pnmh = (LPNMHDR)lParam;
            if (pnmh->idFrom == IDC_ABOUT_LINK && pnmh->code == NM_CLICK) {
                NMLINK* pnmLink = (NMLINK*)lParam;
                ShellExecuteW(hwnd, L"open", pnmLink->item.szUrl, NULL, NULL, SW_SHOWNORMAL);
                return 0;
            }
            break;
        }

        case WM_DESTROY:
            EnableWindow(g_hwndTray, TRUE);
            if (g_hAboutBgBrush) { DeleteObject(g_hAboutBgBrush); g_hAboutBgBrush = NULL; }
            if (g_hTitleFont) { DeleteObject(g_hTitleFont); g_hTitleFont = NULL; }
            if (g_hBodyFont) { DeleteObject(g_hBodyFont); g_hBodyFont = NULL; }
            PostQuitMessage(0); 
            return 0;

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

static void execute_about(void) {
    EnableWindow(g_hwndTray, FALSE);
    AllowSetForegroundWindow(GetCurrentProcessId());

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LINK_CLASS };
    InitCommonControlsEx(&icc);

    const int dlgWidth = 450;
    const int dlgHeight = 220;

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = AboutWindowProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL; 
    wc.lpszClassName = L"JxlShotAboutClass";
    RegisterClassExW(&wc);

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