#ifndef RESOURCE_H
#define RESOURCE_H


// Edit ONLY these three numbers. APP_VERSION and its wide form (shown by the
// About dialog) are derived from them. setup.iss is release metadata that no
// compiler reads; keep it in step when bumping these three numbers.
// (APP_VERSION_NUM/APP_VERSION_STR exist for a VS_VERSIONINFO block, which is
// deliberately omitted - see resource.rc.)

#define APP_VERSION_MAJOR 2
#define APP_VERSION_MINOR 4
#define APP_VERSION_PATCH 7

#define APP_STR_(x) #x
#define APP_STR(x)  APP_STR_(x)

#define APP_VERSION     APP_STR(APP_VERSION_MAJOR) "." APP_STR(APP_VERSION_MINOR) "." APP_STR(APP_VERSION_PATCH)
#define APP_VERSIONW    L"" APP_VERSION   // wide form, used by the About dialog
#define APP_VERSION_NUM APP_VERSION_MAJOR, APP_VERSION_MINOR, APP_VERSION_PATCH, 0
#define APP_VERSION_STR APP_STR(APP_VERSION_MAJOR) ", " APP_STR(APP_VERSION_MINOR) ", " APP_STR(APP_VERSION_PATCH) ", 0"

#define IDI_APP_ICON 1001

#endif // RESOURCE_H