@echo off
SET PLUGIN=fpsvita
SET COMPILER=%SCE_PSP2_SDK_DIR%/host_tools/build/rco/bin/acdc.exe
SET TMP=RES_RCO_TMP

@RD /S /Q "%TMP%"
mkdir "%TMP%"

REM Compile locale string file to RCS
"%COMPILER%" -c -i "locale/fpsvita_locale_en.xml" -s "%SCE_PSP2_SDK_DIR%/host_tools/build/rco/def/rcs.cxmldef" -o "%TMP%/fpsvita_locale_en.xml.rcs"

REM Compile main plugin XML to RCO
"%COMPILER%" -c -i "%PLUGIN%_plugin.xml" -s "%SCE_PSP2_SDK_DIR%/host_tools/build/rco/def/rco.cxmldef" -o "%PLUGIN%_plugin.rco"

echo Built %PLUGIN%_plugin.rco
