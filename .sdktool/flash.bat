@echo off
setlocal enabledelayedexpansion

:: ============================================================
:: 脚本信息
:: ============================================================
set "SCRIPT_VERSION=3.1"
set "SCRIPT_NAME=STM32 Flash Tool (SDK .sdktool)"

:: ============================================================
:: 使用说明
:: ============================================================
:: Usage: flash.bat [JLinkRoot] [ELF] [Device] [Interface] [Speed] [ProjRoot]
::
:: 功能说明:
::   1. 自动从 .ld 文件读取 FLASH ORIGIN，判断 OTA 模式
::   2. OTA 模式: 擦除 APP 区 (保留 BL)，烧录到 ORIGIN
::   3. 普通模式: 不擦除，烧录到默认地址
::   4. 自动查找 .bin 文件和 JLink.exe
::
:: 本脚本是 SDK 单源工具 (my-stm32-sdk/.sdktool/flash.bat)，不随工程拉取
:: (sync_lib.py 只镜像 chip/devices/protocols/middleware)。工程 .vscode 任务
:: 通过相对路径调用本文件，并传入工程根目录 (ProjRoot)，脚本据此定位
:: .ld / .ioc / .bin。省略 ProjRoot 时使用当前工作目录 (VSCode 任务默认
:: cwd = ${workspaceFolder})。

set "JLROOT=%~1"
set "ELF_IN=%~2"
set "DEV=%~3"
set "ITF=%~4"
set "SPEED=%~5"
set "PROJ=%~6"
if "%PROJ%"=="" set "PROJ=%CD%"

:: ============================================================
:: [1/7] 获取时间戳
:: ============================================================
for /f "delims=" %%a in ('powershell -Command "Get-Date -Format 'yyyy-MM-dd HH:mm:ss'"') do set "TIMESTAMP=%%a"
echo [!TIMESTAMP!] ============================================================
echo [!TIMESTAMP!] %SCRIPT_NAME% v%SCRIPT_VERSION%
echo [!TIMESTAMP!] ============================================================

:: ============================================================
:: [2/7] 解析 .ld 文件获取 FLASH 地址 (相对工程根 PROJ)
:: ============================================================
set "LD_FILE="
set "FLASH_ORIGIN="
set "FLASH_LENGTH="
set "OTA_ENABLE=0"

for /f "delims=" %%f in ('dir /b "%PROJ%\*.ld" 2^>nul') do (
    set "LD_FILE=%PROJ%\%%f"
    goto :found_ld
)

:found_ld
if "%LD_FILE%"=="" (
    echo [!TIMESTAMP!] [WARN] No .ld file found, using default
    set "FLASH_ORIGIN=0x08000000"
    set "FLASH_LENGTH=1024K"
    goto :skip_ld_parse
)

echo [!TIMESTAMP!] [INFO] Found linker script: %LD_FILE%

:: 使用 PowerShell 解析 .ld 文件
for /f "delims=" %%a in ('powershell -Command "$content = Get-Content '%LD_FILE%' -Raw; if ($content -match '(?s)FLASH\s*\([^)]+\)\s*:\s*ORIGIN\s*=\s*([^,]+),\s*LENGTH\s*=\s*([^\s]+)') { $matches[1] + '|' + $matches[2] }"') do (
    for /f "tokens=1,2 delims=|" %%b in ("%%a") do (
        set "FLASH_ORIGIN=%%b"
        set "FLASH_LENGTH=%%c"
    )
)

:: 如果解析失败，使用默认值
if "%FLASH_ORIGIN%"=="" (
    echo [!TIMESTAMP!] [WARN] Could not parse FLASH origin, using default
    set "FLASH_ORIGIN=0x08004000"
    set "FLASH_LENGTH=1008K"
) else (
    echo [!TIMESTAMP!] [INFO] FLASH ORIGIN: %FLASH_ORIGIN%
    echo [!TIMESTAMP!] [INFO] FLASH LENGTH: %FLASH_LENGTH%
)

:: --- 自动判断模式 ---
:: 如果 ORIGIN == 0x08000000 -> 普通模式 (BL)
:: 如果 ORIGIN != 0x08000000 -> OTA 模式 (APP)
if /i "%FLASH_ORIGIN%"=="0x08000000" (
    set "OTA_ENABLE=0"
    echo [!TIMESTAMP!] [INFO] Normal mode (ORIGIN == 0x08000000)
) else (
    set "OTA_ENABLE=1"
    echo [!TIMESTAMP!] [INFO] ★ Auto-detected OTA mode (ORIGIN != 0x08000000)
)

:skip_ld_parse
echo [!TIMESTAMP!] ------------------------------------------------------------

:: ============================================================
:: [3/7] 从 .ioc 读取芯片型号 (相对工程根 PROJ)
:: ============================================================
if "%DEV%"=="" (
    set "DEV="
    for %%F in ("%PROJ%\*.ioc") do (
        for /f "tokens=2 delims==" %%A in ('findstr /i "^Mcu" "%%F" 2^>nul') do (
            for /f "tokens=1,2 delims=." %%B in ("%%A") do set "DEV=%%B%%C"
        )
    )
)

if "%DEV%"=="" (
    echo [!TIMESTAMP!] [ERROR] Could not read MCU from .ioc file
    exit /b 1
)
echo [!TIMESTAMP!] [INFO] Device: %DEV%
echo [!TIMESTAMP!] ------------------------------------------------------------

:: ============================================================
:: [4/7] 自动查找 .bin 文件 (相对工程根 PROJ)
:: ============================================================
set "PROJECT_NAME="
set "BIN="

if not "%ELF_IN%"=="" (
    set "BIN_PATH=%~dpn2.bin"
    if exist "!BIN_PATH!" set "BIN=!BIN_PATH!"
    if not defined BIN if exist "%ELF_IN%" set "BIN=%~dpn2.bin"
)

if "%BIN%"=="" (
    if exist "%PROJ%\build\Release\*.bin" (
        for %%F in ("%PROJ%\build\Release\*.bin") do (
            set "BIN=%%F"
            set "PROJECT_NAME=%%~nF"
            goto :found_bin
        )
    )
)

if "%BIN%"=="" (
    if exist "%PROJ%\build\Debug\*.bin" (
        for %%F in ("%PROJ%\build\Debug\*.bin") do (
            set "BIN=%%F"
            set "PROJECT_NAME=%%~nF"
            goto :found_bin
        )
    )
)

if "%BIN%"=="" (
    if exist "%PROJ%\build\*.bin" (
        for %%F in ("%PROJ%\build\*.bin") do (
            set "BIN=%%F"
            set "PROJECT_NAME=%%~nF"
            goto :found_bin
        )
    )
)

:found_bin
if "%BIN%"=="" (
    echo [!TIMESTAMP!] [ERROR] No .bin file found
    echo [!TIMESTAMP!] [ERROR] Please build the project first
    exit /b 1
)

if "%PROJECT_NAME%"=="" (
    for %%F in ("%BIN%") do set "PROJECT_NAME=%%~nF"
)

echo [!TIMESTAMP!] [INFO] Project: %PROJECT_NAME%
echo [!TIMESTAMP!] [INFO] BIN: %BIN%
echo [!TIMESTAMP!] ------------------------------------------------------------

:: ============================================================
:: [5/7] 查找 JLink.exe
:: ============================================================
if "%ITF%"=="" set "ITF=SWD"
if "%SPEED%"=="" set "SPEED=4000"

set "JL="
if not "%JLROOT%"=="" if exist "%JLROOT%\JLink.exe" set "JL=%JLROOT%\JLink.exe"

if "%JL%"=="" (
    for /f "skip=2 tokens=2*" %%A in ('reg query "HKEY_CURRENT_USER\Software\Keil\ARM\JLink" /v "InstallPath" 2^>nul') do (
        if exist "%%B\JLink.exe" set "JL=%%B\JLink.exe"
    )
)

if "%JL%"=="" (
    for /f "skip=2 tokens=2*" %%A in ('reg query "HKEY_LOCAL_MACHINE\SOFTWARE\SEGGER\J-Link" /v "InstallPath" 2^>nul') do (
        if exist "%%B\JLink.exe" set "JL=%%B\JLink.exe"
    )
)

if "%JL%"=="" (
    for /d %%D in ("C:\Program Files\SEGGER\JLink*") do if exist "%%D\JLink.exe" set "JL=%%D\JLink.exe"
)
if "%JL%"=="" (
    for /d %%D in ("C:\Program Files (x86)\SEGGER\JLink*") do if exist "%%D\JLink.exe" set "JL=%%D\JLink.exe"
)

if not exist "%JL%" (
    echo [!TIMESTAMP!] [ERROR] JLink.exe not found
    exit /b 1
)

echo [!TIMESTAMP!] [INFO] JLink: %JL%
echo [!TIMESTAMP!] [INFO] Interface: %ITF%, Speed: %SPEED% kHz
echo [!TIMESTAMP!] ------------------------------------------------------------

:: ============================================================
:: [6/7] 显示最终配置
:: ============================================================
echo [!TIMESTAMP!] [INFO] ========================================
echo [!TIMESTAMP!] [INFO] Project  : %PROJECT_NAME%
echo [!TIMESTAMP!] [INFO] Device   : %DEV%
echo [!TIMESTAMP!] [INFO] BIN      : %BIN%
echo [!TIMESTAMP!] [INFO] Mode     : OTA=%OTA_ENABLE%  (Origin: %FLASH_ORIGIN%)
echo [!TIMESTAMP!] [INFO] ========================================
echo.

:: --- ANSI color ---
for /f %%a in ('echo prompt $E^| cmd') do set "ESC=%%a"

:: ============================================================
:: [7/7] 执行烧录
:: ============================================================
set "SCRIPT=%TEMP%\jlink_%RANDOM%.jlink"
set "LOG=%TEMP%\jlink_flash_%RANDOM%.log"

if "%OTA_ENABLE%"=="1" (
    :: ============================================================
    :: OTA 模式：只擦除 APP 区 (保留 BL)
    :: 擦除范围: FLASH_ORIGIN ~ FLASH_ORIGIN + FLASH_LENGTH
    :: ============================================================
    echo [!TIMESTAMP!] [INFO] OTA mode: Erase APP region and flash to %FLASH_ORIGIN%
    echo [!TIMESTAMP!] [INFO] Erase range: %FLASH_ORIGIN% ~ end of FLASH
    (
        echo r
        echo h
        echo erase %FLASH_ORIGIN%, 0x080FFFFF
        echo loadfile "%BIN%", %FLASH_ORIGIN%
        echo r
        echo g
        echo exit
    ) > "%SCRIPT%"
) else (
    :: ============================================================
    :: 普通模式：不擦除，烧录到默认地址
    :: ============================================================
    echo [!TIMESTAMP!] [INFO] Normal mode: Flash to default address, no erase
    (
        echo r
        echo h
        echo loadfile "%BIN%"
        echo r
        echo g
        echo exit
    ) > "%SCRIPT%"
)

echo [!TIMESTAMP!] [INFO] Flashing "%BIN%" ...
echo.
"%JL%" -device %DEV% -if %ITF% -speed %SPEED% -autoconnect 1 -nogui 1 -CommanderScript "%SCRIPT%" > "%LOG%" 2>&1

type "%LOG%"

:: --- 判断结果 ---
set "FLASH_OK=1"
findstr /i /c:"cannot connect" /c:"could not connect" /c:"error:" /c:"failed" /c:"timeout" /c:"unable to" "%LOG%" >nul && set "FLASH_OK=0"
findstr /c:"O.K." "%LOG%" >nul || set "FLASH_OK=0"

del "%SCRIPT%" 2>nul
del "%LOG%" 2>nul

:: --- 获取结束时间戳 ---
for /f "delims=" %%a in ('powershell -Command "Get-Date -Format 'yyyy-MM-dd HH:mm:ss'"') do set "TIMESTAMP_END=%%a"

echo.
if "%FLASH_OK%"=="1" (
    echo [!TIMESTAMP_END!] %ESC%[30;42m                                                            %ESC%[0m
    echo [!TIMESTAMP_END!] %ESC%[30;42m   ####   FLASH SUCCESS   ####   device=%DEV%                %ESC%[0m
    echo [!TIMESTAMP_END!] %ESC%[30;42m                                                            %ESC%[0m
    echo [!TIMESTAMP_END!] %ESC%[92m^>^>^> Download OK. Target reset and running.%ESC%[0m
    exit /b 0
) else (
    echo [!TIMESTAMP_END!] %ESC%[97;41m                                                            %ESC%[0m
    echo [!TIMESTAMP_END!] %ESC%[97;41m   ####   FLASH FAILED   ####   device=%DEV%                 %ESC%[0m
    echo [!TIMESTAMP_END!] %ESC%[97;41m                                                            %ESC%[0m
    echo [!TIMESTAMP_END!] %ESC%[91m^>^>^> See JLink output above.%ESC%[0m
    exit /b 1
)
