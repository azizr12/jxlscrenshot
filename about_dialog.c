
// about_dialog.c

// Custom About Dialog with full Dark Mode and Window Management control


#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

// Forward declarations to access variables from jxlshot_tray.c
extern HWND g_hwndTray;
extern HWND g_hwndMenuOwner;
extern void ApplyDarkMode(HWND hwnd);

#define IDI_APP_ICON 1001
#define IDC_ABOUT_TITLE 1001
#define IDC_ABOUT_DESCRIPTION 1002
#define IDC_ABOUT_LINK 1003
#define IDC_ABOUT_OK 1004

static HBRUSH g_hAboutBgBrush = NULL;

static INT_PTR CALLBACK AboutDialogProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_INITDIALOG: {
            ApplyDarkMode(hwndDlg);

            HICON hIcon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_APP_ICON));
            if (hIcon) {
                SendMessageW(hwndDlg, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
                SendMessageW(hwndDlg, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
            }

            // Create Title
            CreateWindowExW(0, L"STATIC", L"JXL Screenshot Tool", 
                WS_CHILD | WS_VISIBLE | SS_LEFT, 60, 15, 260, 25, hwndDlg, (HMENU)IDC_ABOUT_TITLE, GetModuleHandleW(NULL), NULL);

            // Create Description
            CreateWindowExW(0, L"STATIC", L"Minimal tray screenshot tool using JPEG XL.", 
                WS_CHILD | WS_VISIBLE | SS_LEFT, 60, 45, 260, 40, hwndDlg, (HMENU)IDC_ABOUT_DESCRIPTION, GetModuleHandleW(NULL), NULL);

            // Create SysLink (Hyperlink)
            CreateWindowExW(0, WC_LINK, L"<a href=\"https://github.com/azizr12/jxlscrenshot\">View on GitHub</a>", 
                WS_CHILD | WS_VISIBLE | LWS_TRANSPARENT, 60, 90, 260, 20, hwndDlg, (HMENU)IDC_ABOUT_LINK, GetModuleHandleW(NULL), NULL);

            // Create OK Button
            CreateWindowExW(0, L"BUTTON", L"OK", 
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP, 120, 130, 100, 28, hwndDlg, (HMENU)IDC_ABOUT_OK, GetModuleHandleW(NULL), NULL);

            if (!g_hAboutBgBrush) {
                g_hAboutBgBrush = CreateSolidBrush(RGB(45, 45, 48)); // Windows Dark Gray (#2D2D30)
            }

            SetFocus(GetDlgItem(hwndDlg, IDC_ABOUT_OK));
            return TRUE;
        }

        case WM_COMMAND:
            if (LOWORD(wParam) == IDC_ABOUT_OK || LOWORD(wParam) == IDCANCEL) {
                DestroyWindow(hwndDlg);
                return TRUE;
            }
            break;

        case WM_NOTIFY: {
            LPNMHDR pnmh = (LPNMHDR)lParam;
            if (pnmh->idFrom == IDC_ABOUT_LINK && pnmh->code == NM_CLICK) {
                LPNMLINK pnmLink = (LPNMLINK)lParam;
                ShellExecuteW(hwndDlg, L"open", pnmLink->item.szUrl, NULL, NULL, SW_SHOWNORMAL);
                return TRUE;
            }
            break;
        }

        case WM_ACTIVATE:
            // If the dialog loses focus, force it back to the foreground
            if (wParam == WA_INACTIVE) {
                SetForegroundWindow(hwndDlg);
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
            PostQuitMessage(0); // Breaks the modal message loop
            return TRUE;

        case WM_CLOSE:
            DestroyWindow(hwndDlg);
            return TRUE;
    }
    return FALSE;
}

void execute_about(void) {
    // Disable the tray window while the dialog is open (modal behavior)
    EnableWindow(g_hwndTray, FALSE);

    // Grant this process the right to set its own windows to the foreground
    AllowSetForegroundWindow(GetCurrentProcessId());

    // Ensure common controls (specifically SysLink) are initialized
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LINK_CLASS };
    InitCommonControlsEx(&icc);

    const int dlgWidth = 340;
    const int dlgHeight = 190;

    // Create the dialog window manually for maximum control over appearance and Z-order
    HWND hwndAbout = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_APPWINDOW | WS_EX_DLGMODALFRAME,
        L"#32770", // Standard Windows dialog class
        L"About",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, dlgWidth, dlgHeight,
        g_hwndMenuOwner,
        NULL,
        GetModuleHandleW(NULL),
        NULL
    );

    if (hwndAbout) {
        // Center the window on the primary monitor work area
        RECT rc;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &rc, 0);
        int x = rc.left + (rc.right - rc.left - dlgWidth) / 2;
        int y = rc.top + (rc.bottom - rc.top - dlgHeight) / 2;
        
        SetWindowPos(hwndAbout, HWND_TOP, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
        
        ShowWindow(hwndAbout, SW_SHOW);
        UpdateWindow(hwndAbout);
        SetForegroundWindow(hwndAbout);

        // Run a local modal message loop
        MSG msg;
        while (GetMessage(&msg, NULL, 0, 0)) {
            if (!IsDialogMessage(hwndAbout, &msg)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
        }
    } else {
        EnableWindow(g_hwndTray, TRUE);
        MessageBoxW(NULL, L"Failed to create About dialog.", L"jxlshot", MB_ICONERROR);
    }
}