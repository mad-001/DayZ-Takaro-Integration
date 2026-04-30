@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
    echo ERROR: could not find Visual Studio 2022 vcvars64.bat
    exit /b 1
)

del Secur32.dll dayz_takaro.dll 2>nul

echo === building dayz_takaro.dll ===
cl.exe /nologo /LD /MD /O2 /DNDEBUG /std:c++17 /EHsc dayz_takaro.cpp ^
    /link /OUT:dayz_takaro.dll winhttp.lib ws2_32.lib
if not exist dayz_takaro.dll (
    echo dayz_takaro.dll build FAILED
    exit /b 2
)

echo === building winmm.dll proxy ===
cl.exe /nologo /LD /MD /O2 /DNDEBUG /std:c++17 /EHsc winmm_proxy.cpp ^
    /link /OUT:winmm.dll
if not exist winmm.dll (
    echo winmm.dll build FAILED
    exit /b 3
)

dir /b *.dll
echo === build OK ===
endlocal
