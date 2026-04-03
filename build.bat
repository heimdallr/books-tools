@echo off

set BUILD_TYPE=Release
call %~dp0configure.bat %*
if %errorlevel% NEQ 0 goto Error

set start_time=%DATE% %TIME%

set BUILD_DIR=%~dp0build\%BUILD_TYPE%
set /p PRODUCT_VERSION=<%BUILD_DIR%\var\version
set /p SEVEN_ZIP_PATH=<%BUILD_DIR%\var\7z

echo building
cmake --build %BUILD_DIR% --config Release
if %errorlevel% NEQ 0 goto Error

cmake --install %BUILD_DIR% --prefix %BUILD_DIR%/flitools

rem echo testing
rem ctest --test-dir %BUILD_DIR% -C Release
rem if %errorlevel% NEQ 0 goto Error

echo installer creating
mkdir %~dp0build\installer

%SEVEN_ZIP_PATH%7z a %~dp0build\installer\flitools_%PRODUCT_VERSION%.7z %BUILD_DIR%\flitools

goto End

:Error
echo someting went wrong :(
exit /B 1

:End
echo working time
echo -- Start: %start_time%
echo -- Stop:  %DATE% %TIME%
