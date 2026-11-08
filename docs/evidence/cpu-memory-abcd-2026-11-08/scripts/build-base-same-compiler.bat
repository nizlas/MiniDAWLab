@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
set NINJA=C:\Users\nicla\AppData\Local\Microsoft\WinGet\Packages\Ninja-build.Ninja_Microsoft.Winget.Source_8wekyb3d8bbwe\ninja.exe
set SRC=C:\Users\nicla\development\MiniDAWLab-rs-base
set BLD=%SRC%\build\ninja-release-base-vs
cmake -S "%SRC%" -B "%BLD%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MAKE_PROGRAM="%NINJA%" -DFETCHCONTENT_SOURCE_DIR_JUCE=%SRC%/build/ninja-release-base/_jsrc -DMINIDAW_ASIO_SDK_DIR=C:/Users/nicla/development/MiniDAWLab/external_tools/asiosdk || exit /b 1
cmake --build "%BLD%" --target MiniDAWLab
