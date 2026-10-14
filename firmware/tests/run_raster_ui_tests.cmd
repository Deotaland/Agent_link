@echo off
where cl >nul 2>nul
if errorlevel 1 exit /b 1
cl /nologo /utf-8 /Zi /W4 /WX /std:c11 /fsanitize=address /c boards\common\raster_montserrat_14.c boards\common\raster_montserrat_20.c boards\common\raster_montserrat_28.c /Fo:build\ /Fd:build\raster_ui_fonts.pdb
if errorlevel 1 exit /b 1
cl /nologo /utf-8 /Zi /EHsc /W4 /WX /std:c++17 /fsanitize=address /Itests\ui_stubs /Iboards\common tests\raster_ui_test.cpp boards\common\stripe_canvas.cc build\raster_montserrat_14.obj build\raster_montserrat_20.obj build\raster_montserrat_28.obj /Fe:build\raster_ui_test.exe /Fo:build\ /Fd:build\raster_ui_compiler.pdb
if errorlevel 1 exit /b 1
build\raster_ui_test.exe
