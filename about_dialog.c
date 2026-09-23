
// about_dialog.c

// Custom About Dialog with full Dark Mode and Window Management control


#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#define IDC_ABOUT_TITLE 1001
#define IDC_ABOUT_DESCRIPTION 1002
#define IDC_ABOUT_LINK 1003
#define IDC_ABOUT_OK 1004

static HBRUSH g_hAboutBgBrush = NULL;

static LRESULT CALLBACK AboutWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_CREATE: {
            ApplyDarkMode(hwnd);

            HICON hIcon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_APP_ICON));
            if (hIcon) {
                SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
                SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
            }

            // Create Title
            CreateWindowExW(0, L"STATIC", L"JXL Screenshot Tool", 
                WS_CHILD | WS_VISIBLE | SS_LEFT, 60, 15, 260, 25, hwnd, (HMENU)IDC_ABOUT_TITLE, GetModuleHandleW(NULL), NULL);

            // Create Description
            CreateWindowExW(0, L"STATIC", L"Minimal tray screenshot tool using JPEG XL.", 
                WS_CHILD | WS_VISIBLE | SS_LEFT, 60, 45, 260, 40, hwnd, (HMENU)IDC_ABOUT_DESCRIPTION, GetModuleHandleW(NULL), NULL);

            // Create SysLink (Hyperlink)
            CreateWindowExW(0, WC_LINK, L"<a href=\"https://github.com/azizr12/jxlscrenshot\">View on GitHub</a>", 
                WS_CHILD | WS_VISIBLE | LWS_TRANSPARENT, 60, 90, 260, 20, hwnd, (HMENU)IDC_ABOUT_LINK, GetModuleHandleW(NULL), NULL);

            // Create OK Button
            CreateWindowExW(0, L"BUTTON", L"OK", 
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP, 120, 130, 100, 28, hwnd, (HMENU)IDC_ABOUT_OK, GetModuleHandleW(NULL), NULL);

            if (!g_hAboutBgBrush) {
                g_hAboutBgBrush = CreateSolidBrush(RGB(45, 45, 48)); // Windows Dark Gray (#2D2D30)
            }
            return 0;
        }

        case WM_ERASEBKGND: {
            // Paint the window background dark gray to prevent white flashing
            HDC hdc = (HDC)wParam;
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, g_hAboutBgBrush);
            return 1; // We handled erasing
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

        case WM_ACTIVATE:
            // If the dialog loses focus, force it back to the foreground
            if (wParam == WA_INACTIVE) {
                SetForegroundWindow(hwnd);
            }
            break;

        case WM_CTLCOLORSTATIC: {
            HDC hdcStatic = (HDC)wParam;
            SetBkColor(hdcStatic, RGB(45, 45, 48));
            SetTextColor(hdcStatic, RGB(255, 255, 255));
            SetBkMode(hdcStatic, TRANSPARENT);
            return (INT_PTR)g_hAboutBgBrush;
        }

        case WM_DESTROY:
            // Re-enable the main tray window when the dialog closes
            EnableWindow(g_hwndTray, TRUE);
            if (g_hAboutBgBrush) {
                DeleteObject(g_hAboutBgBrush);
                g_hAboutBgBrush = NULL;
            }
            PostQuitMessage(0); // Breaks the local modal message loop cleanly
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

    const int dlgWidth = 340;
    const int dlgHeight = 190;

    // 1. Register a dedicated, simple window class for the About dialog
    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = AboutWindowProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL; // We handle background painting in WM_ERASEBKGND
    wc.lpszClassName = L"JxlShotAboutClass";
    RegisterClassExW(&wc);

    // Center the window on the primary monitor work area
    RECT rc;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &rc, 0);
    int x = rc.left + (rc.right - rc.left - dlgWidth) / 2;
    int y = rc.top + (rc.bottom - rc.top - dlgHeight) / 2;

    // 2. Create the window using our registered class
    HWND hwndAbout = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_APPWINDOW | WS_EX_DLGMODALFRAME,
        L"JxlShotAboutClass",
        L"About",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        x, y, dlgWidth, dlgHeight,
        g_hwndMenuOwner,
        NULL,
        GetModuleHandleW(NULL),
        NULL
    );

    if (hwndAbout) {
        ShowWindow(hwndAbout, SW_SHOW);
        UpdateWindow(hwndAbout);
        SetForegroundWindow(hwndAbout);

        // 3. Run a local modal message loop
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