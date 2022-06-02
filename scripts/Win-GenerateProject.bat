@echo off
pushd %~dp0\..\
cmake -Bbuild -G "Visual Studio 17 2022" -DCMAKE_GENERATOR_PLATFORM=x64
popd
PAUSE