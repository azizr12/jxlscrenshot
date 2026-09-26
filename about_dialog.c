// about_dialog.c

// Adobe-Style Frameless Splash About Dialog

#include <windows.h>
#include <windowsx.h> // For GET_X_LPARAM / GET_Y_LPARAM
#include <commctrl.h>
#include <shellapi.h>
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

// Forward declaration
extern void ApplyDarkMode(HWND hwnd);
extern HWND g_hwndTray;
extern HWND g_hwndMenuOwner;

// Close ("X") button hit-box, kept in one place so paint/hit-test/mouse code all agree.
static const RECT kCloseBtnRect = {472, 10, 512, 50};

static LRESULT CALLBACK AboutWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_CREATE: {
            ApplyDarkMode(hwnd);

            // 1. Create Modern Typography (Consolas Regular)
            // FW_NORMAL (400) ensures the "Regular" weight. 
            // FIXED_PITCH | FF_MODERN is the correct flag for monospaced fonts like Consolas.
            // Sizes bumped up slightly for better legibility on the larger window.
            g_hTitleFont = CreateFontW(-27, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
            
            g_hBodyFont = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
            
            g_hXFont = CreateFontW(-22, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
            // 2. Create Background Brush
            g_hAboutBgBrush = CreateSolidBrush(RGB(30, 30, 30));

            // 3. Apply the EXACT SAME 128x128 icon to the window
            SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)g_hAppIcon);
            SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)g_hAppIcon);



            // 4. Layout Controls (Strict 15px vertical spacing between text elements)
            // Text column is kept narrower than the window and stops well short of the
            // icon on the right, so there's a clear gap between the copy and the artwork.

            // Title (Left aligned)
            // Y=40, Height=36 -> Bottom edge = 76
            HWND hTitle = CreateWindowExW(0, L"STATIC", L"JXL Screenshot Tool",
                WS_CHILD | WS_VISIBLE | SS_LEFT, 30, 40, 290, 36, hwnd, (HMENU)IDC_ABOUT_TITLE, GetModuleHandleW(NULL), NULL);
            SendMessageW(hTitle, WM_SETFONT, (WPARAM)g_hTitleFont, TRUE);

            // Build dynamic description with Version and CPU info
            wchar_t desc_text[512];

            // Extract libjxl version.
            // JxlEncoderVersion() returns a DECIMAL-encoded integer: major*1000000 + minor*1000 + patch.
            // (It is NOT byte-packed, so it must not be right-shifted/masked like a Windows FILEVERSION.)
            uint32_t jxl_ver = JxlEncoderVersion();
            int jxl_major = jxl_ver / 1000000;
            int jxl_minor = (jxl_ver / 1000) % 1000;
            int jxl_patch = jxl_ver % 1000;

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

            // Format the final string.
            // Double-space after each label gives a little breathing room without needing tabs.
            _snwprintf(desc_text, 511, // Changed 512 to 511 to leave room for null terminator
                L"Minimal tray screenshot tool using JPEG XL.\n\n"
                L"App Version:  %s\n"
                L"libjxl Version:  %d.%d.%d\n"
                L"CPU Architecture:  x86-64 (%s)",
                APP_VERSIONW, jxl_major, jxl_minor, jxl_patch, cpu_ext);
            
            desc_text[511] = L'\0'; // Guarantee null-termination

            // Description
            // Y=91 (76 + 15px gap), Height=150 -> Bottom edge = 241
            HWND hDesc = CreateWindowExW(0, L"STATIC", desc_text,
                WS_CHILD | WS_VISIBLE | SS_LEFT, 30, 91, 290, 150, hwnd, (HMENU)IDC_ABOUT_DESCRIPTION, GetModuleHandleW(NULL), NULL);
            SendMessageW(hDesc, WM_SETFONT, (WPARAM)g_hBodyFont, TRUE);

            // Hyperlink
            // Y=256 (241 + 15px gap), Height=22 -> Bottom edge = 278
            CreateWindowExW(0, WC_LINK, L"<a href=\"https://github.com/azizr12/jxlscrenshot\">View on GitHub</a>",
                WS_CHILD | WS_VISIBLE | LWS_TRANSPARENT, 30, 256, 250, 22, hwnd, (HMENU)IDC_ABOUT_LINK, GetModuleHandleW(NULL), NULL);

            // Splash Icon
            // Pushed further right (x=350) so there's a clear gap between it and the text column.
            HWND hIconCtrl = CreateWindowExW(0, L"STATIC", L"",
                WS_CHILD | WS_VISIBLE | SS_ICON | SS_CENTERIMAGE, 350, 30, 128, 128, hwnd, (HMENU)IDC_ABOUT_ICON, GetModuleHandleW(NULL), NULL);
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
            RECT rcX = kCloseBtnRect;

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
            if (PtInRect(&kCloseBtnRect, pt)) {
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
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };

            // Check if cursor is within the "X" button bounds
            if (PtInRect(&kCloseBtnRect, pt)) {
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
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };

            // Handle close button click exclusively
            if (PtInRect(&kCloseBtnRect, pt)) {
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
            
            // Clean up GDI objects safely
            if (g_hAboutBgBrush) { DeleteObject(g_hAboutBgBrush); g_hAboutBgBrush = NULL; }
            if (g_hTitleFont) { DeleteObject(g_hTitleFont); g_hTitleFont = NULL; }
            if (g_hBodyFont) { DeleteObject(g_hBodyFont); g_hBodyFont = NULL; }
            if (g_hXFont) { DeleteObject(g_hXFont); g_hXFont = NULL; }
            
            // CRITICAL: DO NOT destroy g_hAppIcon here. 
            // It is shared with the main tray application. Destroying it here 
            // is what causes the "broken icon" bug in the system tray.
            
            // CRITICAL: DO NOT call PostQuitMessage(0) here. 
            // It will terminate your entire tray application when this dialog closes.
            
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

    // ICON LOADING
    // Modern .ico files often skip 128x128 and go straight to 256x256 (PNG compressed).
    // We cascade through sizes to guarantee we get the custom icon, not the generic fallback.
    if (!g_hAppIcon) {
        // Attempt 1: Exact 128x128
        g_hAppIcon = (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 128, 128, LR_DEFAULTCOLOR);
        
        // Attempt 2: Fallback to 256x256 (Windows will smoothly scale this down)
        if (!g_hAppIcon) {
            g_hAppIcon = (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 256, 256, LR_DEFAULTCOLOR);
        }
        
        // Attempt 3: Fallback to default system size if specific sizes are missing
        if (!g_hAppIcon) {
            g_hAppIcon = (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 0, 0, LR_DEFAULTCOLOR);
        }

        // Attempt 4: Absolute fallback to system default
        if (!g_hAppIcon) {
            g_hAppIcon = LoadIconW(NULL, IDI_APPLICATION);
        }
    }

    // Window bumped up slightly (520x300) to accommodate the larger fonts and
    // give the icon more breathing room away from the text column.
    const int dlgWidth = 520;
    const int dlgHeight = 300;

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    
    // Only register the class if it hasn't been registered yet
    if (!GetClassInfoExW(GetModuleHandleW(NULL), L"JxlShotAboutClass", &wc)) {
        wc.lpfnWndProc = AboutWindowProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = NULL;
        wc.lpszClassName = L"JxlShotAboutClass";
        wc.hIcon = g_hAppIcon;
        wc.hIconSm = g_hAppIcon;
        RegisterClassExW(&wc);
    }
    wc.lpfnWndProc = AboutWindowProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"JxlShotAboutClass";

    // Assign the loaded icon to the window class
    wc.hIcon = g_hAppIcon;
    wc.hIconSm = g_hAppIcon;

    RegisterClassExW(&wc);

    RECT rc;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &rc, 0);
    int x = rc.left + (rc.right - rc.left - dlgWidth) / 2;
    int y = rc.top + (rc.bottom - rc.top - dlgHeight) / 2;

    HWND hwndAbout = CreateWindowExW(
        WS_EX_TOPMOST, // Remove WS_EX_APPWINDOW
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
        
        // REMOVED the local while(GetMessage) loop.
        // The main application's message loop will handle this window automatically.
    } else {
        EnableWindow(g_hwndTray, TRUE);
        MessageBoxW(NULL, L"Failed to create About dialog.", L"jxlshot", MB_ICONERROR);
    }
}