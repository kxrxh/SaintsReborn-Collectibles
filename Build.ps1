param([Parameter(Mandatory=$true)][string]$GameRoot,[switch]$Test)
$ErrorActionPreference='Stop'
$vswhere="${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs=& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(!$vs){throw 'Visual Studio C++ build tools are required'}
$vcvars=Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {if($_ -match '^([^=]+)=(.*)$'){Set-Item -Path "env:$($matches[1])" -Value $matches[2]}}
$compiler=Join-Path $GameRoot 'build\llvm-19.1.5\bin\clang-cl.exe'
$common=@('/nologo','/std:c++latest','/EHsc','/O2','/MD',"/I$GameRoot\build\sdk\include","/I$GameRoot\modding\include","/I$GameRoot\build\game\_deps\zlib-src")
$zlib=Join-Path $GameRoot 'build\game\wml_zlib.lib'
if($Test){
    & $compiler @common "$PSScriptRoot\tracker_test.cpp" "/Fo$PSScriptRoot\tracker_test.obj" "/Fe$PSScriptRoot\tracker_test.exe" /link $zlib user32.lib
    if($LASTEXITCODE){throw 'Tracker tests did not compile'}
    & "$PSScriptRoot\tracker_test.exe" $GameRoot
    if($LASTEXITCODE){throw 'Tracker tests failed'}
}else{
    & $compiler @common /LD "$PSScriptRoot\tracker.cpp" "/Fo$PSScriptRoot\CollectiblesTracker.obj" "/Fe$PSScriptRoot\CollectiblesTracker.dll" /link "/IMPLIB:$PSScriptRoot\CollectiblesTracker.lib" $zlib user32.lib
    if($LASTEXITCODE){throw 'Tracker build failed'}
}
