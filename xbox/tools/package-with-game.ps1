# The app and the game in one signed package, for an Xbox with no other way to take the install:
# the SquareEnix folder (FINAL FANTASY XI and the viewer folder) beside the app, where it finds them.
#
#   powershell -File xbox\tools\package-with-game.ps1 -Game "<SquareEnix folder>"
#
# Writes build\xbox\XIonXbox-with-game.msix (about the size of the install; stored, not compressed: the
# game's files barely compress) with build\xbox\XIonXbox.cer. The game's files are read in place (a
# mapping file), not copied. Install it with Device Portal's Add, as the app alone. Never commit it.
param(
    [Parameter(Mandatory = $true)][string]$Game,
    [string]$Configuration = 'Release'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot   # xbox\
$build = Join-Path (Split-Path -Parent $root) 'build\xbox'
foreach ($f in 'FINAL FANTASY XI', 'PlayOnlineViewer') {
    if (-not (Test-Path (Join-Path $Game $f))) { throw "$Game has no $f folder" }
}
$sdkbin = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\10.*" -Directory | Sort-Object Name -Descending |
    ForEach-Object { Join-Path $_.FullName 'x64' } | Where-Object { Test-Path (Join-Path $_ 'makeappx.exe') } | Select-Object -First 1

# the app, built and signed as for an install without the game; its files come from that package
& (Join-Path $PSScriptRoot 'deploy-xbox.ps1') -PackageOnly -Configuration $Configuration
$app = Join-Path $build 'with-game-app'
if (Test-Path $app) { Remove-Item $app -Recurse -Force }
& (Join-Path $sdkbin 'makeappx.exe') unpack /p (Join-Path $build 'XIonXbox-xbox.msix') /d $app /o | Out-Null
if ($LASTEXITCODE) { throw "makeappx unpack failed" }

# the mapping: the app's own files (not the old package's signature, block map or catalog), then the game
$map = New-Object System.Text.StringBuilder
[void]$map.AppendLine('[Files]')
$skip = @('AppxSignature.p7x', 'AppxBlockMap.xml', '[Content_Types].xml')
Get-ChildItem $app -Recurse -File | Where-Object { $skip -notcontains $_.Name -and $_.FullName -notmatch '\\AppxMetadata\\' } | ForEach-Object {
    [void]$map.AppendLine(('"{0}" "{1}"' -f $_.FullName, $_.FullName.Substring($app.Length + 1)))
}
$files = 0; $bytes = 0
foreach ($f in 'FINAL FANTASY XI', 'PlayOnlineViewer') {
    $top = Join-Path $Game $f
    Get-ChildItem $top -Recurse -File -Force | ForEach-Object {
        [void]$map.AppendLine(('"{0}" "SquareEnix\{1}\{2}"' -f $_.FullName, $f, $_.FullName.Substring($top.Length + 1)))
        $files++; $bytes += $_.Length
    }
}
$mapfile = Join-Path $build 'with-game.map'
[IO.File]::WriteAllText($mapfile, $map.ToString(), (New-Object System.Text.UTF8Encoding($false)))
"packing the app and {0:N0} game files, {1:N1} GB" -f $files, ($bytes / 1GB)

$out = Join-Path $build 'XIonXbox-with-game.msix'
& (Join-Path $sdkbin 'makeappx.exe') pack /f $mapfile /p $out /nc /o | Select-Object -Last 3
if ($LASTEXITCODE) { throw "makeappx pack failed" }
& (Join-Path $sdkbin 'signtool.exe') sign /fd SHA256 /f (Join-Path $build 'XIonXbox.pfx') $out | Out-Null
if ($LASTEXITCODE) { throw "signing failed" }
"{0} ({1:N1} GB), with {2}" -f $out, ((Get-Item $out).Length / 1GB), (Join-Path $build 'XIonXbox.cer')
