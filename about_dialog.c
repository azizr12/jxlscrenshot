// about_dialog.c

// Adobe-Style Frameless Splash About Dialog

#include <windows.h>
#include <windowsx.h> // For GET_X_LPARAM / GET_Y_LPARAM
#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h> // For GET_X_LPARAM / GET_Y_LPARAM
#include "resource.h" // for APP_VERSIONW

#define IDC_ABOUT_TITLE 1001
#define IDC_ABOUT_DESCRIPTION 1002
#define IDC_ABOUT_LINK 1003
#define IDC_ABOUT_ICON 1005

static HBRUSH g_hAboutBgBrush = NULL;
static HFONT g_hTitleFont = NULL;
static HFONT g_hBodyFont = NULL;
static HFONT g_hXFont = NULL;
static HICON g_hAppIcon = NULL; // Single global handle for the 128x128 icon

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

            // 4. Apply the EXACT SAME 128x128 icon to the window (Taskbar / Alt-Tab)
            SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)g_hAppIcon);
            SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)g_hAppIcon);

            // 5. Layout Controls
            
            // Title (Left aligned)
            HWND hTitle = CreateWindowExW(0, L"STATIC", L"JXL Screenshot Tool", 
                WS_CHILD | WS_VISIBLE | SS_LEFT, 30, 40, 250, 30, hwnd, (HMENU)IDC_ABOUT_TITLE, GetModuleHandleW(NULL), NULL);
            SendMessageW(hTitle, WM_SETFONT, (WPARAM)g_hTitleFont, TRUE);

            // Build dynamic description with Version and CPU info
            wchar_t desc_text[512];
            
            // Extract libjxl version
            uint32_t jxl_ver = JxlEncoderVersion();
            int jxl_major = (jxl_ver >> 24) & 0xFF;
            int jxl_minor = (jxl_ver >> 16) & 0xFF;
            int jxl_patch = (jxl_ver >> 8) & 0xFF;

            // Define fallback constants for older Windows SDK headers
#ifndef PF_AVX512F_INSTRUCTIONS_AVAILABLE
#define PF_AVX512F_INSTRUCTIONS_AVAILABLE 41
#endif
#ifndef PF_AVX2_INSTRUCTIONS_AVAILABLE
#define PF_AVX2_INSTRUCTIONS_AVAILABLE 34
#endif
#ifndef PF_AVX_INSTRUCTIONS_AVAILABLE
#define PF_AVX_INSTRUCTIONS_AVAILABLE 33
#endif

            // Detect CPU instruction sets
            BOOL has_avx512 = IsProcessorFeaturePresent(PF_AVX512F_INSTRUCTIONS_AVAILABLE);
            BOOL has_avx2 = IsProcessorFeaturePresent(PF_AVX2_INSTRUCTIONS_AVAILABLE);
            BOOL has_avx = IsProcessorFeaturePresent(PF_AVX_INSTRUCTIONS_AVAILABLE);
            
            const wchar_t* cpu_ext = L"SSE2";
            if (has_avx512) cpu_ext = L"AVX-512";
            else if (has_avx2) cpu_ext = L"AVX2";
            else if (has_avx) cpu_ext = L"AVX";

            // Format the final string
            _snwprintf(desc_text, 512, 
                L"Minimal tray screenshot tool using JPEG XL.\n\n"
                L"App Version: %s\n"
                L"libjxl Version: %d.%d.%d\n"
                L"Architecture: x86_64 (%s)", 
                APP_VERSIONW, jxl_major, jxl_minor, jxl_patch, cpu_ext);

            // Description
            HWND hDesc = CreateWindowExW(0, L"STATIC", desc_text, 
                WS_CHILD | WS_VISIBLE | SS_LEFT, 30, 80, 250, 110, hwnd, (HMENU)IDC_ABOUT_DESCRIPTION, GetModuleHandleW(NULL), NULL);
            SendMessageW(hDesc, WM_SETFONT, (WPARAM)g_hBodyFont, TRUE);

            // Hyperlink
            CreateWindowExW(0, WC_LINK, L"<a href=\"https://github.com/azizr12/jxlscrenshot\">View on GitHub</a>", 
                WS_CHILD | WS_VISIBLE | LWS_TRANSPARENT, 30, 200, 200, 20, hwnd, (HMENU)IDC_ABOUT_LINK, GetModuleHandleW(NULL), NULL);
            
            // Splash Icon
            HWND hIconCtrl = CreateWindowExW(0, L"STATIC", L"", 
                WS_CHILD | WS_VISIBLE | SS_ICON | SS_CENTERIMAGE, 300, 30, 128, 128, hwnd, (HMENU)IDC_ABOUT_ICON, GetModuleHandleW(NULL), NULL);
            SendMessageW(hIconCtrl, STM_SETICON, (WPARAM)g_hAppIcon, 0);

            return 0;
        }

        case WM_ERASEBKGND: {
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
            
            FillRect(hdc, &rc, g_hAboutBgBrush);
            
            SetBkMode(hdc, TRANSPARENT);
            RECT rcX = {440, 10, 480, 50};
            
            // Always use the default "X" color without hover effects
            SetTextColor(hdc, RGB(255, 85, 85));
            
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
                SetTextColor(hdcStatic, RGB(255, 255, 255));
            } else if (id == IDC_ABOUT_DESCRIPTION) {
                SetTextColor(hdcStatic, RGB(160, 160, 160));
            }
            
            return (INT_PTR)g_hAboutBgBrush;
        }

        case WM_NCHITTEST: {
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hwnd, &pt);
            
            // 1. Close button area: Treat as client so WM_LBUTTONDOWN can handle the click
            RECT rcX = {440, 10, 480, 50};
            if (PtInRect(&rcX, pt)) {
                return HTCLIENT;
            }
            
            // 2. Interactive controls (e.g., hyperlink): Treat as client so they can be clicked
            HWND hChild = ChildWindowFromPoint(hwnd, pt);
            if (hChild != NULL && hChild != hwnd) {
                int id = GetDlgCtrlID(hChild);
                if (id == IDC_ABOUT_LINK) {
                    return HTCLIENT; 
                }
            }
            
            // 3. Everywhere else acts as the title bar (fully draggable by the OS)
            return HTCAPTION;
        }

        case WM_MOUSEMOVE: {
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            
            // Check if cursor is within the "X" button bounds
            if (x >= 440 && x <= 480 && y >= 10 && y <= 50) {
                SetCursor(LoadCursorW(NULL, IDC_HAND));
                
                // Request a WM_MOUSELEAVE message when the cursor exits the window
                TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&tme);
            } else {
                SetCursor(LoadCursorW(NULL, IDC_ARROW));
            }
            return 0;
        }

        case WM_MOUSELEAVE: {
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
            
            // Handle close button click exclusively
            if (x >= 440 && x <= 480 && y >= 10 && y <= 50) {
                DestroyWindow(hwnd);
                return 0;
            }
            
            // Dragging is fully handled by WM_NCHITTEST returning HTCAPTION.
            // Manual ReleaseCapture / SendMessage is not needed.
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
    EnableWindow(g_hwndTray, FALSE);
    AllowSetForegroundWindow(GetCurrentProcessId());

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LINK_CLASS };
    InitCommonControlsEx(&icc);

    // Load the 128x128 icon ONCE to be used universally for Splash Screen and Taskbar
    if (!g_hAppIcon) {
        g_hAppIcon = (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 128, 128, 0);
    }

    const int dlgWidth = 480;
    const int dlgHeight = 240;

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = AboutWindowProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL; 
    wc.lpszClassName = L"JxlShotAboutClass";
    
    // Assign the EXACT SAME 128x128 icon to the window class
    // Windows will automatically scale it down for the taskbar button
    wc.hIcon = g_hAppIcon;
    wc.hIconSm = g_hAppIcon;
    
    RegisterClassExW(&wc);

    RECT rc;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &rc, 0);
    int x = rc.left + (rc.right - rc.left - dlgWidth) / 2;
    int y = rc.top + (rc.bottom - rc.top - dlgHeight) / 2;

    HWND hwndAbout = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_APPWINDOW,
        L"JxlShotAboutClass",
        L"About",
        WS_POPUP | WS_VISIBLE,
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