// custom_theme.c
// Custom dark-themed UI components (Menu, About Dialog, and Dark Mode Support)

#define UNICODE
#define _UNICODE
#define WINVER 0x0601
#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN

#include "resource.h"
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <windowsx.h>
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

void InitializeDarkMode(void) {
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

void ApplyDarkMode(HWND hwnd) {
    InitializeDarkMode();
    if (!hwnd || !g_pAllowDarkModeForWindow) return;

    g_pAllowDarkModeForWindow(hwnd, TRUE);
    
    // Crucial: Tell the window and its children to redraw with the new theme
    SendMessageW(hwnd, WM_THEMECHANGED, 0, 0);
    RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
}

// Expose cleanup for WinMain
void CleanupUxtheme(void) {
    if (g_hUxtheme) {
        FreeLibrary(g_hUxtheme);
        g_hUxtheme = NULL;
    }
}

static HWND g_hwndMenuOwner = NULL;

// Tray Icon & Context Menu

void show_tray_menu(HWND hwnd) {
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

void init_custom_ui(HINSTANCE hInst, HWND hwndTray) {
    // Initialize dark mode BEFORE creating any windows or dialogs
    InitializeDarkMode();

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
}

void destroy_custom_ui(void) {
    if (g_hwndMenuOwner) {
        DestroyWindow(g_hwndMenuOwner);
        g_hwndMenuOwner = NULL;
    }
}

// Pulls in the custom About dialog implementation
#include "about_dialog.c"