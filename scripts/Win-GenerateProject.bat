@echo off
pushd %~dp0\..\
cmake -Hc:/Users/ole/source/repos/OloEngineBase -Bc:/Users/ole/source/repos/OloEngineBase/build -G "Visual Studio 17 2022" -DCMAKE_GENERATOR_PLATFORM=x64
popd
PAUSE