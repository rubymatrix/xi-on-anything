# Builds the app and installs it on this PC as a loose layout (Windows developer mode required).
#
#   powershell -File xbox\tools\deploy.ps1 [-Configuration Release] [-Launch]
#
# Build the game first: python tools\build.py uwp-lib.
# The unsigned package the build writes is unpacked into build\xbox\layout and registered from there, so
# a rebuild and rerun replaces the installed app in place. To reach a server on this same PC the app
# also needs a loopback exemption, once, from an administrator prompt:
#
#   CheckNetIsolation LoopbackExempt -a -n=XIonXbox_<publisher id>   (printed below)
param(
    [string]$Configuration = 'Release',
    [switch]$Launch
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot   # xbox\
$build = Join-Path (Split-Path -Parent $root) 'build\xbox'
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -prerelease -property installationPath
$msbuild = Join-Path $vs 'MSBuild\Current\Bin\amd64\MSBuild.exe'

& $msbuild (Join-Path $root 'app\XIonXbox.vcxproj') "-p:Configuration=$Configuration" -p:Platform=x64 -m -nologo -v:m
if ($LASTEXITCODE) { throw "build failed" }

$package = Get-ChildItem (Join-Path $build 'packages') -Recurse -Filter 'XIonXbox_*_x64.msix' |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $package) { throw "no package under build\xbox\packages" }
$sdkbin = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\10.*" -Directory | Sort-Object Name -Descending |
    ForEach-Object { Join-Path $_.FullName 'x64\makeappx.exe' } | Where-Object { Test-Path $_ } | Select-Object -First 1

$layout = Join-Path $build 'layout'
# Updated in place: uninstalling (Remove-AppxPackage) would delete the app's data - LocalState (logs,
# settings.reg, the game's USER folder, the pipeline cache) and the saved login.
Get-Process XIonXbox -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500
& $sdkbin unpack /p $package.FullName /d $layout /o | Out-Null
if ($LASTEXITCODE) { throw "makeappx unpack failed" }
Remove-Item (Join-Path $layout 'AppxSignature.p7x') -ErrorAction SilentlyContinue
Remove-Item (Join-Path $layout 'AppxBlockMap.xml') -ErrorAction SilentlyContinue
Remove-Item -LiteralPath (Join-Path $layout '[Content_Types].xml') -ErrorAction SilentlyContinue
Add-AppxPackage -Register (Join-Path $layout 'AppxManifest.xml') -ForceApplicationShutdown -ForceUpdateFromAnyVersion

$app = Get-AppxPackage -Name XIonXbox
"installed $($app.PackageFullName)"
"loopback exemption (administrator, once): CheckNetIsolation LoopbackExempt -a -n=$($app.PackageFamilyName)"
if ($Launch) { Start-Process "shell:AppsFolder\$($app.PackageFamilyName)!App" }
