@echo off
REM Compile the ray tracing and denoiser shaders to SPIR-V with the Vulkan SDK's
REM glslangValidator. Runs as the pre-build step; can also be run by hand.
REM The *.glsl files are shared includes and are not compiled on their own.

setlocal
cd /d "%~dp0shaders"

set GLSLANG="%VULKAN_SDK%\Bin\glslangValidator.exe"
if not exist %GLSLANG% (
    echo ERROR: glslangValidator not found. Is the Vulkan SDK installed and VULKAN_SDK set?
    exit /b 1
)

echo Compiling shaders with %GLSLANG%

for %%S in (raygen_rt.rgen raygen_pt.rgen miss.rmiss shadow.rmiss closesthit.rchit atrous.comp) do (
    %GLSLANG% --target-env vulkan1.2 -V "%%S" -o "%%S.spv"
    if errorlevel 1 (
        echo ERROR compiling %%S
        exit /b 1
    )
    echo   %%S -^> %%S.spv
)

echo Shaders compiled successfully.
endlocal
exit /b 0
