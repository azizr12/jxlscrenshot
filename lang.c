// lang.c — external language packs for jxlshot.
//
// Every user-visible string is looked up by a key through tr(). The English
// text is compiled in as the fallback, so the application always works even
// when no language file is installed. A language is exactly one INI file:
//
//   <exe folder>\lang\en.ini
//   <exe folder>\lang\ar.ini
//
//   [Language]
//   Name=العربية          ; shown in the Settings window
//   RTL=1                 ; 1 = right-to-left layout for this language
//   Font=Segoe UI         ; optional UI font override
//
//   [Strings]
//   MenuExit=خروج
//   SaveErrorBody=...\n...  ; "\n" is unescaped to a line break
//
// Adding a language later means copying lang\en.ini, translating the values
// and saving it as lang\<code>.ini. No source change is needed: the file is
// discovered automatically, and its Name shows up in Settings > Language.
//
// Selection:
//   jxlshot.ini  [Capture]  Language=auto   (default: Windows UI language)
//   Language=ar                             (force a language)
//   Language=en                             (force English)
//
// Unknown keys in a language file are ignored; keys missing from it fall back
// to the built-in English text below, so an incomplete translation is safe.

#include <wchar.h>
#include <string.h>   // _wcsicmp

#define LANG_MAX_ENTRIES 96
#define LANG_KEY_LEN     48
#define LANG_TEXT_LEN    768
#define LANG_MAX_FILES   16

typedef struct {
    const wchar_t *key;
    const wchar_t *en;
} LangDefault;

typedef struct {
    wchar_t key[LANG_KEY_LEN];
    wchar_t text[LANG_TEXT_LEN];
} LangEntry;

typedef struct {
    wchar_t code[32];
    wchar_t name[64];
} LangFile;

// Master key list. Adding a string to the app = adding one row here (the
// English text is both the fallback and the translation template).
static const LangDefault kLangDefaults[] = {
    { L"AppName",           L"JXL Shot" },
    { L"AppTitle",          L"jxlshot" },

    // Tray menu
    { L"MenuOpenGui",       L"Open Capture Tool" },
    { L"MenuFull",          L"Capture Full Screen" },
    { L"MenuRegion",        L"Capture Region" },
    { L"MenuSetPath",       L"Set Export Path..." },
    { L"MenuOpenExport",    L"Open Export Folder" },
    { L"MenuOpenConfig",    L"Open Config File" },
    { L"MenuSettings",      L"Settings" },
    { L"MenuDebug",         L"Debug: %s" },
    { L"MenuReleases",      L"Open Releases Page" },
    { L"MenuAbout",         L"About..." },
    { L"MenuExit",          L"Exit" },
    { L"On",                L"ON" },
    { L"Off",               L"OFF" },

    // Capture Tool strip
    { L"BtnFull",           L"Full Screen" },
    { L"BtnRegion",         L"Region" },
    { L"BtnSettings",       L"Settings" },
    { L"BtnOpenFolder",     L"Open Folder" },
    { L"ThemeDark",         L"Dark" },
    { L"ThemeLight",        L"Light" },

    // Settings window
    { L"HkTitle",           L"Settings" },
    { L"HkHintIdle",        L"Click a button, then press the new key combo." },
    { L"HkHintCapture",     L"Press the new key combo. Esc cancels." },
    { L"HkSectionJxl",      L"JPEG XL" },
    { L"HkDistance",        L"Distance" },
    { L"HkEffort",          L"Effort" },
    { L"HkSectionSystem",   L"System" },
    { L"HkLanguage",        L"Language:  %s" },
    { L"LangAuto",          L"Automatic (%s)" },
    { L"LangRestart",       L"Language: %s\n\nRestart jxlshot to apply the new language." },
    { L"HkIniInfo",         L"Open .ini file for more infos" },
    { L"HkFullPrompt",      L"Full Screen:  press a key..." },
    { L"HkRegionPrompt",    L"Region:  press a key..." },
    { L"HkFullLabel",       L"Full Screen:  %s" },
    { L"HkRegionLabel",     L"Region:  %s" },
    { L"LosslessOn",        L"Lossless:  ON" },
    { L"LosslessOff",       L"Lossless:  OFF (lossy)" },
    { L"SetExportFolder",   L"Set Export Folder..." },
    { L"PreviewOn",         L"Open In Viewer:  ON" },
    { L"PreviewOff",        L"Open In Viewer:  OFF" },
    { L"HookEnabled",       L"Keyboard Hook:  Enabled" },
    { L"HookDisabled",      L"Keyboard Hook:  Disabled" },

    // Hotkey name building
    { L"HotkeyNone",        L"None (Click to set)" },
    { L"ModCtrl",           L"Ctrl+" },
    { L"ModShift",          L"Shift+" },
    { L"ModAlt",            L"Alt+" },
    { L"ModWin",            L"Win+" },
    { L"HotkeyPrintScreen", L"PrintScreen" },
    { L"HotkeyF",           L"F%d" },
    { L"HotkeyKey",         L"Key" },

    // Messages
    { L"NoMonitors",        L"No monitors found." },
    { L"CaptureFailed",     L"Screen capture failed." },
    { L"EncodeFailed",      L"Encoding or saving failed." },
    { L"ExportPathTitle",   L"Select Export Folder for Screenshots" },
    { L"ExportPathUpdated", L"Export path updated." },
    { L"GuiCreateFailed",   L"Failed to create the jxlshot window." },
    { L"TrayInitFailed",    L"Failed to initialize the tray application window class." },
    { L"TrayCreateFailed",  L"Failed to create the tray window." },
    { L"SaveErrorTitle",    L"jxlshot Save Error" },
    { L"SaveErrorBody",
      L"Failed to save screenshot to:\n%s\n\nFallback to Pictures folder:\n%s\nalso failed.\n\n"
      L"Please check disk space and permissions." },
    { L"SaveFallbackTitle", L"jxlshot Fallback Save" },
    { L"SaveFallbackBody",
      L"Failed to save to configured export path.\n\n"
      L"Screenshot was successfully saved to fallback location:\n%s" },

    // About dialog
    { L"AboutBody",
      L"A minimal screenshot tool using JPEG XL.\n\n"
      L"App Version:  %s\n"
      L"libjxl Version:  %d.%d.%d\n"
      L"CPU Architecture:  x86-64 (%s)" },
    { L"AboutLink",         L"<a href=\"https://github.com/azizr12/jxlshot\">View on GitHub</a>" },
    { L"AboutWindow",       L"About" },
    { L"AboutError",        L"Failed to create About dialog." },
};

#define LANG_DEFAULT_COUNT ((int)(sizeof(kLangDefaults) / sizeof(kLangDefaults[0])))

static LangEntry g_lang[LANG_MAX_ENTRIES];
static int       g_lang_count = 0;

static wchar_t   g_lang_code[32]     = L"en";        // active language
static wchar_t   g_lang_name[64]     = L"English";   // active display name
static wchar_t   g_lang_font[LF_FACESIZE] = L"Segoe UI";
static int       g_lang_rtl          = 0;
static int       g_lang_auto         = 1;            // INI says "auto"
static wchar_t   g_lang_selected[32] = L"";          // explicit selection, "" for auto

static LangFile  g_lang_files[LANG_MAX_FILES];
static int       g_lang_file_count = 0;

static void lang_lang_path(wchar_t *out, int n, const wchar_t *code) {
    _snwprintf(out, (size_t)n, L"%s\\lang\\%s.ini", g_exe_dir, code);
    if (n > 0) out[n - 1] = L'\0';
}

// INI values are single-line, so line breaks are stored as "\n" (and a
// literal backslash as "\\"). Translators only need to keep those escapes.
static void lang_unescape(wchar_t *dst, size_t cap, const wchar_t *src) {
    if (!cap) return;
    size_t d = 0;
    for (size_t s = 0; src[s] && d + 1 < cap; s++) {
        if (src[s] == L'\\' && src[s + 1]) {
            wchar_t c = src[++s];
            if (c == L'n') c = L'\n';
            else if (c == L't') c = L'\t';
            // "\\" and any other "\x" produce the literal character x.
            dst[d++] = c;
        } else {
            dst[d++] = src[s];
        }
    }
    dst[d] = L'\0';
}

// Fill the entry table from a language file. Keys missing from the file (or
// present but empty) keep the built-in English text.
static void lang_load_values(const wchar_t *path) {
    wchar_t buf[LANG_TEXT_LEN];

    for (int i = 0; i < LANG_DEFAULT_COUNT && i < LANG_MAX_ENTRIES; i++) {
        // A sentinel default tells "key missing" apart from "value is empty".
        GetPrivateProfileStringW(L"Strings", kLangDefaults[i].key, L"\x01",
                                 buf, LANG_TEXT_LEN, path);
        if (buf[0] == L'\x01' || buf[0] == L'\0') {
            wcsncpy(g_lang[i].text, kLangDefaults[i].en, LANG_TEXT_LEN - 1);
            g_lang[i].text[LANG_TEXT_LEN - 1] = L'\0';
        } else {
            wchar_t unescaped[LANG_TEXT_LEN];
            lang_unescape(unescaped, LANG_TEXT_LEN, buf);
            wcsncpy(g_lang[i].text, unescaped, LANG_TEXT_LEN - 1);
            g_lang[i].text[LANG_TEXT_LEN - 1] = L'\0';
        }
    }
}

// Reads [Capture] Language into g_lang_auto / g_lang_selected. Only the
// selection is refreshed when the user picks a language; the visible strings
// change on the next start (layout direction cannot change in place).
static void lang_read_selection(void) {
    wchar_t sel[64];
    GetPrivateProfileStringW(L"Capture", L"Language", L"auto", sel, 64, g_ini_path);

    if (_wcsicmp(sel, L"auto") == 0 || sel[0] == L'\0') {
        g_lang_auto = 1;
        g_lang_selected[0] = L'\0';
        return;
    }

    g_lang_auto = 0;
    wcsncpy(g_lang_selected, sel, 31);
    g_lang_selected[31] = L'\0';

    // Accept both "ar" and "ar.ini" in the INI.
    size_t len = wcslen(g_lang_selected);
    if (len > 4 && _wcsicmp(g_lang_selected + len - 4, L".ini") == 0)
        g_lang_selected[len - 4] = L'\0';
}

static void lang_scan_files(void) {
    g_lang_file_count = 0;

    wchar_t mask[MAX_PATH];
    _snwprintf(mask, MAX_PATH, L"%s\\lang\\*.ini", g_exe_dir);
    mask[MAX_PATH - 1] = L'\0';

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(mask, &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        if (g_lang_file_count >= LANG_MAX_FILES) break;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        wchar_t code[32];
        wcsncpy(code, fd.cFileName, 31);
        code[31] = L'\0';
        wchar_t *dot = wcsrchr(code, L'.');
        if (dot) *dot = L'\0';
        if (!code[0]) continue;

        LangFile *f = &g_lang_files[g_lang_file_count];
        wcsncpy(f->code, code, 31);
        f->code[31] = L'\0';

        wchar_t path[MAX_PATH];
        lang_lang_path(path, MAX_PATH, code);
        GetPrivateProfileStringW(L"Language", L"Name", code, f->name, 64, path);
        if (!f->name[0]) wcsncpy(f->name, code, 63);
        f->name[63] = L'\0';

        g_lang_file_count++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    // Stable, human order for the Settings cycle.
    for (int i = 1; i < g_lang_file_count; i++) {
        LangFile tmp = g_lang_files[i];
        int j = i - 1;
        while (j >= 0 && _wcsicmp(g_lang_files[j].name, tmp.name) > 0) {
            g_lang_files[j + 1] = g_lang_files[j];
            j--;
        }
        g_lang_files[j + 1] = tmp;
    }
}

// Called once at startup, after init_paths()/ensure_default_ini() have run.
void lang_init(void) {
    // 1. Start from the built-in English text, so every key is always valid.
    g_lang_count = 0;
    for (int i = 0; i < LANG_DEFAULT_COUNT && i < LANG_MAX_ENTRIES; i++) {
        wcsncpy(g_lang[i].key, kLangDefaults[i].key, LANG_KEY_LEN - 1);
        g_lang[i].key[LANG_KEY_LEN - 1] = L'\0';
        wcsncpy(g_lang[i].text, kLangDefaults[i].en, LANG_TEXT_LEN - 1);
        g_lang[i].text[LANG_TEXT_LEN - 1] = L'\0';
        g_lang_count++;
    }

    wcscpy(g_lang_code, L"en");
    wcscpy(g_lang_name, L"English");
    wcscpy(g_lang_font, L"Segoe UI");
    g_lang_rtl = 0;

    // 2. Resolve which language is wanted.
    lang_read_selection();

    wchar_t code[32];
    if (!g_lang_auto) {
        wcsncpy(code, g_lang_selected, 31);
        code[31] = L'\0';
    } else {
        // Windows UI language as a plain ISO 639 code ("en", "ar", "fr", ...).
        code[0] = L'\0';
        if (!GetLocaleInfoW(LOCALE_USER_DEFAULT, LOCALE_SISO639LANGNAME,
                            code, (int)(sizeof(code) / sizeof(code[0])))) {
            code[0] = L'\0';
        }
    }

    // 3. Load the file when present. Without a file (or for "en") the
    //    built-in English text stays in place.
    if (code[0]) {
        wchar_t path[MAX_PATH];
        lang_lang_path(path, MAX_PATH, code);

        if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
            lang_load_values(path);

            wchar_t buf[64];
            GetPrivateProfileStringW(L"Language", L"Name", code, buf, 64, path);
            wcsncpy(g_lang_name, buf[0] ? buf : code, 63);
            g_lang_name[63] = L'\0';

            wcsncpy(g_lang_code, code, 31);
            g_lang_code[31] = L'\0';

            g_lang_rtl = GetPrivateProfileIntW(L"Language", L"RTL", 0, path) ? 1 : 0;

            GetPrivateProfileStringW(L"Language", L"Font", L"Segoe UI", g_lang_font,
                                     LF_FACESIZE, path);
            if (!g_lang_font[0]) wcscpy(g_lang_font, L"Segoe UI");
            g_lang_font[LF_FACESIZE - 1] = L'\0';
        }
    }

    // 4. Discover every installed pack for Settings > Language.
    lang_scan_files();
}

// The one lookup used by the UI. Missing keys return an empty string rather
// than English text, which makes a typo visible instead of silently mixing
// two languages (all real keys come from kLangDefaults).
const wchar_t *tr(const wchar_t *key) {
    if (!key) return L"";
    for (int i = 0; i < g_lang_count; i++) {
        if (wcscmp(g_lang[i].key, key) == 0) return g_lang[i].text;
    }
    return L"";
}

const wchar_t *lang_code(void)   { return g_lang_code; }
const wchar_t *lang_name(void)   { return g_lang_name; }
const wchar_t *lang_font_name(void) { return g_lang_font; }
int lang_is_rtl(void)            { return g_lang_rtl; }
int lang_is_auto(void)           { return g_lang_auto; }
const wchar_t *lang_selected_code(void) { return g_lang_selected; }

// Re-reads only [Capture] Language after the Settings window wrote it.
void lang_refresh_selection(void) { lang_read_selection(); }

int lang_file_count(void) { return g_lang_file_count; }const wchar_t *lang_file_code(int i) {
    if (i < 0 || i >= g_lang_file_count) return L"";
    return g_lang_files[i].code;
}
const wchar_t *lang_file_name(int i) {
    if (i < 0 || i >= g_lang_file_count) return L"";
    return g_lang_files[i].name;
}

// Display name for an explicit selection, falling back to the code itself.
const wchar_t *lang_display_name(const wchar_t *code) {
    for (int i = 0; i < g_lang_file_count; i++) {
        if (_wcsicmp(g_lang_files[i].code, code) == 0) return g_lang_files[i].name;
    }
    return code;
}
