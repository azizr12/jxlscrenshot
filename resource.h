#ifndef RESOURCE_H
#define RESOURCE_H


// Edit ONLY these three numbers. APP_VERSION and its wide form (shown by the
// About dialog) are derived from them. VERSION and setup.iss are release
// metadata that no compiler reads; keep them in step when bumping these three
// numbers.

#define APP_VERSION_MAJOR 2
#define APP_VERSION_MINOR 3
#define APP_VERSION_PATCH 7

#define APP_STR_(x) #x
#define APP_STR(x)  APP_STR_(x)

#define APP_VERSION     APP_STR(APP_VERSION_MAJOR) "." APP_STR(APP_VERSION_MINOR) "." APP_STR(APP_VERSION_PATCH)
#define APP_VERSIONW    L"" APP_VERSION   // wide form, used by the About dialog
#define APP_VERSION_NUM APP_VERSION_MAJOR, APP_VERSION_MINOR, APP_VERSION_PATCH, 0
#define APP_VERSION_STR APP_STR(APP_VERSION_MAJOR) ", " APP_STR(APP_VERSION_MINOR) ", " APP_STR(APP_VERSION_PATCH) ", 0"

#define IDI_APP_ICON 1001

// System Tray Menu Command IDs
#define ID_TRAY_CAPTURE_FULL    4001
#define ID_TRAY_CAPTURE_REGION  4002
#define ID_TRAY_OPEN_CONFIG     4003
#define ID_TRAY_CHECK_UPDATE    4004
#define ID_TRAY_EXIT            4005

#endif // RESOURCE_H