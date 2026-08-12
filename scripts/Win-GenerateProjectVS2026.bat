@echo off
rem Generate the Visual Studio 2026 solution WITHOUT CMakePresets. Equivalent to
rem `cmake --preset msvc`; kept for parity with the VS2022 script.
rem
rem Because it bypasses the presets, the vcpkg toolchain has to be supplied here —
rem see docs/ops/build.md. VCPKG_ROOT must point at a bootstrapped vcpkg clone.
if "%VCPKG_ROOT%"=="" (
    echo.
    echo ERROR: VCPKG_ROOT is not set.
    echo   Third-party dependencies come from the vcpkg manifest ^(vcpkg.json^) since issue #773.
    echo   Set it up once with:
    echo       git clone https://github.com/microsoft/vcpkg D:\vcpkg
    echo       D:\vcpkg\bootstrap-vcpkg.bat
    echo       setx VCPKG_ROOT D:\vcpkg
    echo       git -C D:\vcpkg config core.fsmonitor false
    echo   Full instructions: docs\ops\build.md
    echo.
    PAUSE
    exit /b 1
)
pushd %~dp0\..\
cmake -Bbuild -G "Visual Studio 18 2026" -DCMAKE_GENERATOR_PLATFORM=x64 ^
    -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake" ^
    -DVCPKG_TARGET_TRIPLET=x64-windows-static-md ^
    -DVCPKG_HOST_TRIPLET=x64-windows-static-md ^
    -DVCPKG_OVERLAY_TRIPLETS=cmake/triplets ^
    -DVCPKG_OVERLAY_PORTS=cmake/overlay-ports
popd
PAUSE
