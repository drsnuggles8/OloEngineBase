@echo off
pushd %~dp0\..\
cmake -Hc:/Users/ole/source/repos/OloEngine -Bc:/Users/ole/source/repos/OloEngine/build -G "Visual Studio 17 2022" -DCMAKE_GENERATOR_PLATFORM=x64
popd
PAUSE