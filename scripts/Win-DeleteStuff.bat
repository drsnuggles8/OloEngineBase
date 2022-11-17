@echo off
pushd %~dp0\..\
RMDIR "bin" /S /Q
RMDIR "build" /S /Q
popd
PAUSE