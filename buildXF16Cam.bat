@echo off
set "BUILD_VARIANT=%~1"
if "%BUILD_VARIANT%"=="" set "BUILD_VARIANT=ptz"
set "BASE_VARIANT=%BUILD_VARIANT:_netlog=%"
set "BASE_VARIANT=%BASE_VARIANT:_talk=%"
if /I not "%BASE_VARIANT%"=="ptz" if /I not "%BASE_VARIANT%"=="no_ptz" (
	echo Usage: %~nx0 [ptz^|no_ptz][_talk][_netlog]   e.g. ptz, no_ptz_talk, ptz_talk_netlog
	exit /b 1
)


rd /S /Q dist

docker build --build-arg BUILD_VARIANT=%BUILD_VARIANT% -t xf16cam-build . && ^
docker create --name xf16cam-extract xf16cam-build && ^
docker cp xf16cam-extract:/workspace/dist ./dist && ^
docker rm xf16cam-extract

pause
