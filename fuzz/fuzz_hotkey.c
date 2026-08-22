#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <ctype.h>

// Mock Windows types and constants for Linux compilation
typedef unsigned int UINT;
typedef int BOOL;
#define TRUE 1
#define FALSE 0

#define VK_SNAPSHOT 0x2C
#define VK_SCROLL 0x91
#define VK_PAUSE 0x13
#define VK_CAPITAL 0x14
#define VK_NUMLOCK 0x90
#define VK_SPACE 0x20
#define VK_ESCAPE 0x1B
#define VK_RETURN 0x0D
#define VK_TAB 0x09
#define VK_BACK 0x08
#define VK_INSERT 0x2D
#define VK_DELETE 0x2E
#define VK_HOME 0x24
#define VK_END 0x23
#define VK_PRIOR 0x21
#define VK_NEXT 0x22
#define VK_UP 0x26
#define VK_DOWN 0x28
#define VK_LEFT 0x25
#define VK_RIGHT 0x27
#define VK_F1 0x70
#define VK_NUMPAD0 0x60
#define VK_NUMPAD9 0x69
#define VK_MULTIPLY 0x6A
#define VK_ADD 0x6B
#define VK_SUBTRACT 0x6D
#define VK_DECIMAL 0x6E
#define VK_DIVIDE 0x6F

#define MOD_CONTROL 0x0002
#define MOD_SHIFT 0x0004
#define MOD_ALT 0x0001
#define MOD_WIN 0x0008

// Cross-platform mocks for MSVC-specific functions
static int mock_wtoi(const wchar_t *str) {
    return (int)wcstol(str, NULL, 10);
}

static int mock_wcsicmp(const wchar_t *s1, const wchar_t *s2) {
    while (*s1 && *s2) {
        wchar_t c1 = towlower((wint_t)*s1);
        wchar_t c2 = towlower((wint_t)*s2);
        if (c1 != c2) return c1 - c2;
        s1++; s2++;
    }
    return towlower((wint_t)*s1) - towlower((wint_t)*s2);
}

// --- Logic extracted from jxlshot.c ---
static UINT parse_vk(const wchar_t *key) {
    if (!key || !*key) return 0;
    if (mock_wcsicmp(key, L"PrintScreen") == 0 || mock_wcsicmp(key, L"ImprEcran") == 0 || mock_wcsicmp(key, L"PrtScn") == 0) return VK_SNAPSHOT;
    if (mock_wcsicmp(key, L"ScrollLock") == 0) return VK_SCROLL;
    if (mock_wcsicmp(key, L"Pause") == 0 || mock_wcsicmp(key, L"Break") == 0) return VK_PAUSE;
    if (mock_wcsicmp(key, L"CapsLock") == 0) return VK_CAPITAL;
    if (mock_wcsicmp(key, L"NumLock") == 0) return VK_NUMLOCK;
    if (mock_wcsicmp(key, L"Space") == 0 || mock_wcsicmp(key, L"Spacebar") == 0) return VK_SPACE;
    if (mock_wcsicmp(key, L"Escape") == 0 || mock_wcsicmp(key, L"Esc") == 0) return VK_ESCAPE;
    if (mock_wcsicmp(key, L"Enter") == 0 || mock_wcsicmp(key, L"Return") == 0) return VK_RETURN;
    if (mock_wcsicmp(key, L"Tab") == 0) return VK_TAB;
    if (mock_wcsicmp(key, L"Backspace") == 0 || mock_wcsicmp(key, L"Back") == 0) return VK_BACK;
    if (mock_wcsicmp(key, L"Insert") == 0 || mock_wcsicmp(key, L"Ins") == 0) return VK_INSERT;
    if (mock_wcsicmp(key, L"Delete") == 0 || mock_wcsicmp(key, L"Del") == 0) return VK_DELETE;
    if (mock_wcsicmp(key, L"Home") == 0) return VK_HOME;
    if (mock_wcsicmp(key, L"End") == 0) return VK_END;
    if (mock_wcsicmp(key, L"PageUp") == 0 || mock_wcsicmp(key, L"PgUp") == 0) return VK_PRIOR;
    if (mock_wcsicmp(key, L"PageDown") == 0 || mock_wcsicmp(key, L"PgDn") == 0) return VK_NEXT;
    if (mock_wcsicmp(key, L"Up") == 0) return VK_UP;
    if (mock_wcsicmp(key, L"Down") == 0) return VK_DOWN;
    if (mock_wcsicmp(key, L"Left") == 0) return VK_LEFT;
    if (mock_wcsicmp(key, L"Right") == 0) return VK_RIGHT;
    if (towlower((wint_t)key[0]) == L'f') {
        int n = mock_wtoi(key + 1);
        if (n >= 1 && n <= 24) return VK_F1 + n - 1;
    }
    if (key[1] == L'0') {
        return (UINT)towupper((wint_t)key[0]);
    }
    if (mock_wcsicmp(key, L"NumPad0") == 0) return VK_NUMPAD0;
    if (mock_wcsicmp(key, L"NumPad1") == 0) return VK_NUMPAD0 + 1;
    if (mock_wcsicmp(key, L"NumPad2") == 0) return VK_NUMPAD0 + 2;
    if (mock_wcsicmp(key, L"NumPad3") == 0) return VK_NUMPAD0 + 3;
    if (mock_wcsicmp(key, L"NumPad4") == 0) return VK_NUMPAD0 + 4;
    if (mock_wcsicmp(key, L"NumPad5") == 0) return VK_NUMPAD0 + 5;
    if (mock_wcsicmp(key, L"NumPad6") == 0) return VK_NUMPAD0 + 6;
    if (mock_wcsicmp(key, L"NumPad7") == 0) return VK_NUMPAD0 + 7;
    if (mock_wcsicmp(key, L"NumPad8") == 0) return VK_NUMPAD0 + 8;
    if (mock_wcsicmp(key, L"NumPad9") == 0) return VK_NUMPAD9;
    if (mock_wcsicmp(key, L"Multiply") == 0) return VK_MULTIPLY;
    if (mock_wcsicmp(key, L"Add") == 0) return VK_ADD;
    if (mock_wcsicmp(key, L"Subtract") == 0) return VK_SUBTRACT;
    if (mock_wcsicmp(key, L"Decimal") == 0) return VK_DECIMAL;
    if (mock_wcsicmp(key, L"Divide") == 0) return VK_DIVIDE;
    return 0;
}

static BOOL parse_hotkey(const wchar_t *str, UINT *mod, UINT *vk) {
    *mod = 0; *vk = 0;
    if (!str || !*str) return FALSE;
    wchar_t buf[256];
    wcsncpy(buf, str, 255); buf[255] = L'\0';
    wchar_t *p = buf;
    wchar_t *token;
    while (1) {
        token = wcschr(p, L'+');
        if (token) *token = L'\0';
        while (*p == L' ') p++;
        wchar_t *end = p + wcslen(p) - 1;
        while (end > p && *end == L' ') { *end = L'\0'; end--; }
        if (mock_wcsicmp(p, L"Ctrl") == 0) *mod |= MOD_CONTROL;
        else if (mock_wcsicmp(p, L"Shift") == 0) *mod |= MOD_SHIFT;
        else if (mock_wcsicmp(p, L"Alt") == 0) *mod |= MOD_ALT;
        else if (mock_wcsicmp(p, L"Win") == 0) *mod |= MOD_WIN;
        else *vk = parse_vk(p);
        if (!token) break;
        p = token + 1;
    }
    return (*vk != 0);
}

// Fuzz target entry point for libFuzzer
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size == 0) return 0;
    
    size_t wchar_count = size / sizeof(wchar_t);
    if (wchar_count == 0) return 0;
    
    wchar_t *input = (wchar_t *)malloc((wchar_count + 1) * sizeof(wchar_t));
    if (!input) return 0;
    
    memcpy(input, data, wchar_count * sizeof(wchar_t));
    input[wchar_count] = L'\0';
    
    UINT mod = 0;
    UINT vk = 0;
    
    // Execute the target function
    parse_hotkey(input, &mod, &vk);
    
    free(input);
    return 0;
}
