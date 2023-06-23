@echo off
pushd %~dp0\..\
if exist "bin" (
    RMDIR "bin" /S /Q
)
if exist "build" (
    RMDIR "build" /S /Q
)
if exist "OloEngine\vendor" (
    for /d %%i in ("OloEngine\vendor\*") do (
        if /i not "%%i"=="OloEngine\vendor\CMakeLists.txt" (
            RMDIR "%%i" /S /Q
        )
    )
)
popd
PAUSE