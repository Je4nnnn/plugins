param([switch]$Test)
$ErrorActionPreference = 'Stop'
$projectRoot = $PSScriptRoot
$compilerSetup = $null
$visualStudioRoots = @(
    "${env:ProgramFiles(x86)}\Microsoft Visual Studio",
    "$env:ProgramFiles\Microsoft Visual Studio"
)
foreach ($studioRoot in $visualStudioRoots) {
    if (Test-Path -LiteralPath $studioRoot) {
        $matches = @(Get-ChildItem -LiteralPath $studioRoot -Filter vcvars64.bat -Recurse -File -ErrorAction SilentlyContinue)
        if ($matches.Count -gt 0) { $compilerSetup = $matches[-1].FullName; break }
    }
}
if (-not $compilerSetup) { throw 'Install Visual Studio Build Tools with Desktop development with C++ and Windows SDK.' }
$buildDirectory = Join-Path $projectRoot 'build'
$distDirectory = Join-Path $projectRoot 'dist'
New-Item -ItemType Directory -Path $buildDirectory, $distDirectory -Force | Out-Null
$batchPath = Join-Path $buildDirectory 'compile.cmd'
$batch = @"
@echo off
call "$compilerSetup" >nul
if errorlevel 1 exit /b 1
cd /d "$projectRoot"
cl /nologo /std:c++17 /utf-8 /W4 /O2 /MT /EHsc /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /LD /Fo"build\\" src\Core.cpp src\Engine.cpp src\State.cpp src\Editor.cpp src\Plugin.cpp /link /NOLOGO /MACHINE:X64 /DEF:src\BeatWeaver.def /OUT:dist\BeatWeaver_x64.dll /IMPLIB:build\BeatWeaver.lib user32.lib gdi32.lib gdiplus.lib comdlg32.lib shell32.lib
if errorlevel 1 exit /b 1
exit /b 0
"@
[System.IO.File]::WriteAllText($batchPath, $batch, [System.Text.Encoding]::ASCII)
Push-Location $projectRoot
try {
    & $env:ComSpec /d /c "`"$batchPath`""
    if ($LASTEXITCODE -ne 0) { throw "C++ compilation failed ($LASTEXITCODE)." }
    Write-Host "Built $distDirectory\BeatWeaver_x64.dll"
    if ($Test) {
        & python tests\validate_plugin.py
        if ($LASTEXITCODE -ne 0) { throw 'Plug-in validation failed.' }
    }
} finally { Pop-Location }
