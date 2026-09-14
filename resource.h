#ifndef RESOURCE_H
#define RESOURCE_H

// This string is used by the update checker to compare against the remote VERSION file.
#define APP_VERSION "2.2.0"
#define APP_VERSIONW L"2.2.0" // wide-character compatibility

// Numeric version for Windows file properties
#define APP_VERSION_NUM 2,2,0,0
#define APP_VERSION_STR "2, 2, 0, 0"

#define IDI_APP_ICON 1001

// System Tray Menu Command IDs
#define ID_TRAY_CAPTURE_FULL    4001
#define ID_TRAY_CAPTURE_REGION  4002
#define ID_TRAY_OPEN_CONFIG     4003
#define ID_TRAY_CHECK_UPDATE    4004
#define ID_TRAY_EXIT            4005

#endif // RESOURCE_H