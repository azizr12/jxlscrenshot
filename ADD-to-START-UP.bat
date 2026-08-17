@echo off
:: ---------------------------------------------------------
:: Configuration
:: ---------------------------------------------------------
:: Change '*.exe' to the exact name of your executable file.
set "APP_FILENAME=jxlshot_tray.exe"

:: Change '*' to the desired name for the registry entry.
set "REG_ENTRY_NAME=jxlshot_tray"

:: ---------------------------------------------------------
:: Execution
:: ---------------------------------------------------------
:: Determine the full path of the application based on the batch file's location
set "FULL_APP_PATH=%~dp0%APP_FILENAME%"

:: Add the application to the current user's startup registry
reg add "HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run" /v "%REG_ENTRY_NAME%" /t REG_SZ /d "\"%FULL_APP_PATH%\"" /f

:: Verify the result and provide feedback
if %errorlevel% equ 0 (
    echo Successfully added "%APP_FILENAME%" to the startup registry.
) else (
    echo Failed to add the application to startup. Please verify the file name and permissions.
)

pause
