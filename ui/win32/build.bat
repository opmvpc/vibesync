@echo off
rem build.bat — construction du client Windows handmade (VS-014).
rem   build.bat        release  : build\vibesync.exe (GUI, -O2 -s, warnings stricts)
rem   build.bat test   tests    : build\vibesync_tests.exe puis EXECUTION
rem   build.bat asan   tests sous AddressSanitizer
rem   build.bat capture        : captures d'ecran PNG des deux ecrans
setlocal
set "ROOT=%~dp0"
set "CC=C:\Users\thibs\tools\llvm-mingw\bin\x86_64-w64-mingw32-clang.exe"
set "RC=C:\Users\thibs\tools\llvm-mingw\bin\x86_64-w64-mingw32-windres.exe"
if not exist "%CC%" set "CC=x86_64-w64-mingw32-clang.exe"
if not exist "%RC%" set "RC=x86_64-w64-mingw32-windres.exe"

rem Version : source unique = fichier VERSION a la racine du depot. Elle est
rem passee en jeton brut (0.2.0 est un pp-number valide) et transformee en
rem chaine par le preprocesseur cote C : aucun echappement de guillemets a
rem travers cmd.exe, donc rien qui casse selon le shell.
set "VERSION=dev"
if exist "%ROOT%..\..\VERSION" set /p VERSION=<"%ROOT%..\..\VERSION"
rem CORE_DIR = la couche C COMMUNE aux deux clients natifs (ADR-010, VS-031).
rem Elle a quitte ui\win32\src pour la racine du depot : c'est le meme
rem repertoire que compile la cible SwiftPM VSCore du client macOS, sans
rem duplication ni lien symbolique (cf. Package.swift a la racine).
set "CORE_DIR=%ROOT%..\..\core"
set "STD=-std=c11 -ffp-contract=off -DVIBESYNC_VERSION_RAW=%VERSION%"
set "WARN=-Wall -Wextra -Werror -Wshadow -Wvla -Wstrict-prototypes -Wmissing-prototypes"
set "INC=-I "%CORE_DIR%\include" -I "%ROOT%src""
set "LIBS=-lwinhttp -lws2_32 -lbcrypt -lgdi32 -luser32 -lole32 -luuid -lshell32 -lcrypt32"
rem Chaque chemin est entre guillemets : un checkout dans un dossier contenant
rem des espaces doit compiler sans bricolage.
rem
rem CORE_SHARED = le C PORTABLE (ADR-010) : aucun windows.h, aucun wchar_t, tout
rem texte en UTF-8. C'est exactement ce que compile VSCore ; ce repertoire ne
rem doit accueillir que des fichiers qui compilent tels quels sur macOS.
set "CORE_SHARED="%CORE_DIR%\src\base_core.c" "%CORE_DIR%\src\json.c" "%CORE_DIR%\src\protocol.c" "%CORE_DIR%\src\engine.c" "%CORE_DIR%\src\conn.c" "%CORE_DIR%\src\vlc_core.c" "%CORE_DIR%\src\ini_core.c" "%CORE_DIR%\src\media_core.c""
rem CORE_WIN32 = ce qui ne quittera jamais Windows : arenes/journal/UTF-16,
rem Winsock, WinHTTP, DPAPI, GDI. Pendant exact de core\posix (macOS), qui
rem n'est JAMAIS compile ici.
set "CORE_WIN32="%ROOT%src\base_win32.c" "%ROOT%src\vlc_win32.c" "%ROOT%src\net.c" "%ROOT%src\ini_win32.c" "%ROOT%src\health.c" "%ROOT%src\secret.c" "%ROOT%src\media_win32.c" "%ROOT%src\ui.c""
set "CORE=%CORE_SHARED% %CORE_WIN32%"
rem La moitie portable de la suite vit avec le core (core\tests) ; test_win32.c
rem et l'ordonnanceur test_main.c restent ici.
set "TESTS="%ROOT%src\test_main.c" "%CORE_DIR%\tests\test_core.c" "%ROOT%src\test_win32.c""
rem test_util.h (harnais partage) vit avec la moitie portable de la suite.
set "TESTINC=-I "%CORE_DIR%\tests""
set "VECTORS=%ROOT%..\..\test\vectors"
if not exist "%ROOT%build" mkdir "%ROOT%build"

if /I "%~1"=="test" goto :test
if /I "%~1"=="asan" goto :asan
if /I "%~1"=="capture" goto :capture
if /I "%~1"=="clean" goto :clean
if "%~1"=="" goto :release
echo cible inconnue "%~1" (cibles : ^<vide^>, test, asan, capture, clean)
exit /b 2

:release
echo [release] version %VERSION%
echo [release] ressources
"%RC%" "%ROOT%vibesync.rc" -O coff -o "%ROOT%build\vibesync.res" -I "%ROOT%"
if errorlevel 1 exit /b 1
echo [release] vibesync.exe
rem -mwindows : application graphique (pas de console) ; -municode : wWinMain.
"%CC%" %STD% %WARN% %INC% -O2 -s -mwindows -municode -o "%ROOT%build\vibesync.exe" %CORE% "%ROOT%src\main.c" "%ROOT%build\vibesync.res" %LIBS%
if errorlevel 1 exit /b 1
for %%F in ("%ROOT%build\vibesync.exe") do echo         %%~zF octets
exit /b 0

:test
echo [test] compilation
"%CC%" %STD% %WARN% %INC% %TESTINC% -O1 -g -o "%ROOT%build\vibesync_tests.exe" %CORE% %TESTS% %LIBS%
if errorlevel 1 exit /b 1
echo [test] execution
"%ROOT%build\vibesync_tests.exe" "%VECTORS%"
if errorlevel 1 (
  echo [test] ECHEC
  exit /b 1
)
echo [test] OK
exit /b 0

:asan
echo [asan] compilation
"%CC%" %STD% %WARN% %INC% %TESTINC% -O1 -g -fsanitize=address -fno-omit-frame-pointer -o "%ROOT%build\vibesync_tests_asan.exe" %CORE% %TESTS% %LIBS%
if errorlevel 1 exit /b 1
echo [asan] execution
set "PATH=C:\Users\thibs\tools\llvm-mingw\bin;%PATH%"
"%ROOT%build\vibesync_tests_asan.exe" "%VECTORS%"
if errorlevel 1 (
  echo [asan] ECHEC
  exit /b 1
)
echo [asan] OK
exit /b 0

:capture
if not exist "%ROOT%build\vibesync.exe" call "%ROOT%build.bat"
set "OUT=%~2"
if "%OUT%"=="" set "OUT=%ROOT%..\..\docs\research\captures"
if not exist "%OUT%" mkdir "%OUT%"
"%ROOT%build\vibesync.exe" --capture "%OUT%"
echo [capture] PNG dans %OUT%
exit /b 0

:clean
if exist "%ROOT%build" rmdir /s /q "%ROOT%build"
echo [clean] fait
exit /b 0
