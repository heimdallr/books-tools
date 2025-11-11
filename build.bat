@echo off

set BUILD_TYPE=Release
call %~dp0solution_generate.bat %*
if %errorlevel% NEQ 0 goto Error

set start_time=%DATE% %TIME%

echo building
cmake --build %BUILD_DIR% --config Release
if %errorlevel% NEQ 0 goto Error

rem echo testing
rem ctest --test-dir %BUILD_DIR% -C Release
rem if %errorlevel% NEQ 0 goto Error

echo installer creating
mkdir %~dp0build\installer

set /p SEVEN_ZIP_PATH=<%BUILD_DIR%\path\7z

%SEVEN_ZIP_PATH%7z a %~dp0build\installer\tools_%PRODUCT_VERSION%.7z %~dp0build\%BUILD_TYPE%\bin\*

goto End

:Error
echo someting went wrong :(
exit /B 1

:End
echo working time
echo -- Start: %start_time%
echo -- Stop:  %DATE% %TIME%
