@echo off
setlocal EnableExtensions EnableDelayedExpansion
set "ESC="
for /f "tokens=2 delims=#" %%E in ('"prompt #$E# & for %%A in (1) do rem"') do set "ESC=%%E"
if defined ESC (
    echo ESC captured, len test:
    echo !ESC![0;32mGREEN TEXT!ESC![0m plain
) else (
    echo ESC NOT captured
)
exit /b 0
