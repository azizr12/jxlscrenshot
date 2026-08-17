# jxlshot_hotkeys.py
import sys
import os
import configparser

try:
    import keyboard
    import win32gui
    import win32con
except ImportError:
    print("ERROR: Missing required libraries.")
    print("Please install them by running: pip install keyboard pywin32")
    sys.exit(1)

# Constants matching jxlshot_tray.c
WINDOW_CLASS = "JxlShotTrayClass"
WM_COMMAND = 0x0111
IDM_FULL = 101
IDM_REGION = 102

def get_ini_path():
    """Locate jxlshot.ini in the same directory as this script/executable."""
    if getattr(sys, 'frozen', False):
        # Running as compiled PyInstaller binary
        base_path = os.path.dirname(sys.executable)
    else:
        # Running as standard Python script
        base_path = os.path.dirname(os.path.abspath(__file__))
    return os.path.join(base_path, 'jxlshot.ini')

def ini_to_keyboard_format(hk_str):
    """Convert INI format (e.g., 'Ctrl+PrintScreen') to keyboard library format."""
    if not hk_str:
        return None
    parts = hk_str.split('+')
    formatted = []
    for part in parts:
        part = part.strip().lower()
        if part in ('printscreen', 'imprecran'):
            formatted.append('print screen')
        elif part == 'scrolllock':
            formatted.append('scroll lock')
        else:
            formatted.append(part)
    return '+'.join(formatted)

def send_command(cmd_id):
    """Send WM_COMMAND to the tray application."""
    hwnd = win32gui.FindWindow(WINDOW_CLASS, None)
    if hwnd:
        win32gui.PostMessage(hwnd, WM_COMMAND, cmd_id, 0)
    else:
        print("[!] jxlshot_tray.exe is not running. Please start it first.")

def trigger_full():
    send_command(IDM_FULL)

def trigger_region():
    send_command(IDM_REGION)

def main():
    ini_path = get_ini_path()
    config = configparser.ConfigParser()
    config.read(ini_path)

    # Read hotkeys from INI, fallback to defaults if not found
    hk_full_raw = config.get('Capture', 'HotkeyFull', fallback='PrintScreen')
    hk_region_raw = config.get('Capture', 'HotkeyRegion', fallback='Ctrl+PrintScreen')

    hk_full = ini_to_keyboard_format(hk_full_raw)
    hk_region = ini_to_keyboard_format(hk_region_raw)

    print(f"[*] JXL Shot Hotkey Listener Started")
    print(f"[*] Full Screen Hotkey : '{hk_full}'")
    print(f"[*] Region Hotkey      : '{hk_region}'")
    print("[*] Press Ctrl+C in this terminal to exit.")

    if hk_full:
        keyboard.add_hotkey(hk_full, trigger_full)
    if hk_region:
        keyboard.add_hotkey(hk_region, trigger_region)

    try:
        # Block indefinitely, consuming minimal CPU
        keyboard.wait() 
    except KeyboardInterrupt:
        print("\n[*] Exiting hotkey listener.")
        keyboard.unhook_all()

if __name__ == "__main__":
    # Warn if not running as Administrator (required for low-level hooks on Windows)
    import ctypes
    if not ctypes.windll.shell32.IsUserAnAdmin():
        print("[!] WARNING: Global hotkeys (especially PrintScreen) require Administrator privileges on Windows.")
        print("[!] Please run this script/binary as Administrator.")
    
    main()
