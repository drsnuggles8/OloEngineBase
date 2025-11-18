@echo off
pushd %~dp0\..\
cmake -Bbuild -G "Visual Studio 18 2026" -DCMAKE_GENERATOR_PLATFORM=x64
popd
PAUSE