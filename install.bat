@echo off
setlocal enabledelayedexpansion
rem Installs Thistle on Windows: clones the engine (there's no prebuilt
rem package -- see docs/building.md, you vendor the source and build it
rem every time, on purpose) to a stable location, then puts a `thistle`
rem command on your PATH. Windows counterpart of install.sh -- same
rem defaults, same env var names, so docs/instructions don't have to fork
rem per platform.
rem
rem Usage:
rem   install.bat                     (run locally, e.g. after cloning)
rem   powershell -c "iwr https://raw.githubusercontent.com/StacikM/thistle/main/install.bat -OutFile install.bat"  then install.bat
rem
rem Env overrides:
rem   THISTLE_HOME     where the engine lives (default: %USERPROFILE%\.thistle)
rem   THISTLE_BIN_DIR  where the `thistle` command goes (default: %USERPROFILE%\.local\bin)

set "REPO_URL=https://github.com/StacikM/thistle.git"

if "%THISTLE_HOME%"=="" (
    set "INSTALL_DIR=%USERPROFILE%\.thistle"
) else (
    set "INSTALL_DIR=%THISTLE_HOME%"
)
if "%THISTLE_BIN_DIR%"=="" (
    set "BIN_DIR=%USERPROFILE%\.local\bin"
) else (
    set "BIN_DIR=%THISTLE_BIN_DIR%"
)

where git >nul 2>nul
if errorlevel 1 (
    echo thistle: error: git is required ^(used to fetch the engine source^) 1>&2
    exit /b 1
)

rem Prefer `python`, the common name for the official python.org installer on
rem Windows -- fall back to the `py` launcher (also installed by python.org)
rem if `python` isn't on PATH but py is.
set "PYTHON_CMD=python"
where python >nul 2>nul
if errorlevel 1 (
    where py >nul 2>nul
    if errorlevel 1 (
        echo thistle: error: Python 3 is required ^(the CLI is stdlib-only, but it does need an interpreter^) 1>&2
        echo Install it from https://python.org and re-run this script. 1>&2
        exit /b 1
    )
    set "PYTHON_CMD=py"
)

if exist "%INSTALL_DIR%\.git" (
    echo ==^> Updating existing install at %INSTALL_DIR%
    git -C "%INSTALL_DIR%" pull --ff-only
    if errorlevel 1 exit /b 1
) else (
    echo ==^> Cloning Thistle to %INSTALL_DIR%
    git clone --depth 1 "%REPO_URL%" "%INSTALL_DIR%"
    if errorlevel 1 exit /b 1
)

if not exist "%BIN_DIR%" mkdir "%BIN_DIR%"

rem No symlinks: creating one on Windows needs Developer Mode or an elevated
rem prompt, neither of which this script should require. A one-line wrapper
rem that just execs the real script is simpler and needs no privileges --
rem same trick tools\thistle-editor's CMakeLists avoids needing elevation for.
> "%BIN_DIR%\thistle.bat" (
    echo @echo off
    echo %PYTHON_CMD% "%INSTALL_DIR%\tools\thistle-cli\thistle.py" %%*
)

echo ==^> Engine: %INSTALL_DIR%
echo ==^> CLI:    %BIN_DIR%\thistle.bat
echo.

rem Wrapped in semicolons so this is an exact PATH-segment match, not a
rem substring match that "C:\bin" would wrongly satisfy against "C:\binfoo"
rem -- same reasoning as install.sh's ":$PATH:" case check.
echo ;%PATH%; | findstr /i /c:";%BIN_DIR%;" >nul
if errorlevel 1 (
    echo %BIN_DIR% isn't on your PATH yet. Add it with:
    echo.
    echo     setx PATH "%%PATH%%;%BIN_DIR%"
    echo.
    echo Then open a new terminal and try: thistle new mygame ^&^& cd mygame ^&^& thistle run
) else (
    echo Try: thistle new mygame ^&^& cd mygame ^&^& thistle run
)

endlocal
