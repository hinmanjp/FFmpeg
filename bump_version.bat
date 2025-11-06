@echo off
REM Bump version and create tarball
REM Usage: bump_version.bat alpha-5 "Description of changes"

if "%~1"=="" (
    echo Usage: bump_version.bat ^<version^> ^<description^>
    echo Example: bump_version.bat alpha-5 "Fixed mutex crash"
    exit /b 1
)

set VERSION=8.0-zmq-%~1
set DESCRIPTION=%~2

echo Updating RELEASE to %VERSION%...
echo %VERSION% > RELEASE

echo.
echo Committing changes...
git add -A
git commit -m "%DESCRIPTION%"

echo.
echo Creating tag v%VERSION%...
git tag -a v%VERSION% -m "%DESCRIPTION%"
git push origin expand_zmq_support

git push origin v%VERSION%

echo.
echo Creating tarball...
REM git archive --format=tar.gz --prefix=ffmpeg/ -o ffmpeg-expand_zmq_support.tar.gz HEAD

echo.
echo ========================================
echo Done! Version %VERSION%
echo ========================================
echo.

