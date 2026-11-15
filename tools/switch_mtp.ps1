# Copies a folder to a Switch connected over USB as an MTP device (DBI's "MTP responder", or another
# homebrew MTP server), for tools/build_switch.py setup --mtp. Windows gives an MTP device no drive
# letter, so this goes through the Shell's portable-device folders.
#
#   powershell -File tools/switch_mtp.ps1 -Source <folder> -Dest switch/xi-on-anything [-Device Switch] [-Storage "1: SD Card"]
#
# Only files missing on the card, or of another size, are sent: a copy that stopped part way, or a game
# update, sends only what is missing or changed. A folder whose files are all missing goes in one Shell
# copy (much faster over MTP than file by file). -List prints the device's storages and exits.
param(
    [string]$Source,
    [string]$Dest,
    [string]$Device = 'Switch',
    [string]$Storage = '1: SD Card',
    [switch]$List
)
$ErrorActionPreference = 'Stop'
$shell = New-Object -ComObject Shell.Application
# no progress window, yes to all, no folder confirmation, no error dialog
$FLAGS = 4 -bor 16 -bor 512 -bor 1024

function Child($folder, $name) {
    foreach ($i in $folder.Items()) { if ($i.Name -eq $name) { return $i } }
    return $null
}

$dev = $null
foreach ($i in $shell.NameSpace(17).Items()) { if ($i.Name -eq $Device) { $dev = $i } }
if (-not $dev) { throw "no MTP device named '$Device' (is the Switch connected, with DBI's MTP responder running?)" }
if ($List) {
    foreach ($s in $dev.GetFolder.Items()) {
        $free = $s.ExtendedProperty('System.FreeSpace')
        if ($free) { '{0} ({1:N1} GB free)' -f $s.Name, ($free / 1GB) } else { $s.Name }
    }
    exit 0
}
$store = Child $dev.GetFolder $Storage
if (-not $store) { throw "'$Device' has no storage '$Storage' (-List shows them)" }

function Ensure-Folder($parent, $name) {
    $c = Child $parent $name
    if ($c) { return $c.GetFolder }
    $parent.NewFolder($name)
    for ($t = 0; $t -lt 100; $t++) {
        $c = Child $parent $name
        if ($c) { return $c.GetFolder }
        Start-Sleep -Milliseconds 100
    }
    throw "could not make the folder '$name' on the device"
}

$script:sent = 0; $script:bytes = 0; $script:checked = 0; $script:start = Get-Date

# waits until every name in $want (name -> size) is on the device with that size
function Wait-For($folder, $want) {
    $total = ($want.Values | Measure-Object -Sum).Sum
    $deadline = (Get-Date).AddSeconds(60 + $total / 2MB)  # 2 MB/s at worst, and a minute to start
    while ($true) {
        $have = @{}
        foreach ($i in $folder.Items()) { if (-not $i.IsFolder) { $have[$i.Name] = [int64]$i.ExtendedProperty('System.Size') } }
        $missing = @($want.Keys | Where-Object { -not $have.ContainsKey($_) -or $have[$_] -ne $want[$_] })
        if ($missing.Count -eq 0) { return }
        if ((Get-Date) -gt $deadline) { throw "timed out waiting for $($missing.Count) files in a folder (first: $($missing[0]))" }
        Start-Sleep -Milliseconds 200
    }
}

function Mirror($localDir, $devFolder) {
    $have = @{}
    foreach ($i in $devFolder.Items()) { if (-not $i.IsFolder) { $have[$i.Name] = [int64]$i.ExtendedProperty('System.Size') } }
    $files = @(Get-ChildItem -LiteralPath $localDir -File)
    $want = @{}
    foreach ($f in $files) {
        $script:checked++
        if (-not $have.ContainsKey($f.Name) -or $have[$f.Name] -ne $f.Length) { $want[$f.Name] = [int64]$f.Length }
    }
    if ($want.Count -gt 0) {
        if ($want.Count -eq $files.Count -and $have.Count -eq 0) {
            # the folder's files, all of them, in one copy
            $items = $shell.NameSpace($localDir).Items()
            $items.Filter(64 -bor 128, '*')  # SHCONTF_NONFOLDERS | SHCONTF_INCLUDEHIDDEN
            $devFolder.CopyHere($items, $FLAGS)
        } else {
            foreach ($n in $want.Keys) { $devFolder.CopyHere((Join-Path $localDir $n), $FLAGS) }
        }
        Wait-For $devFolder $want
        $script:sent += $want.Count
        $script:bytes += ($want.Values | Measure-Object -Sum).Sum
        $secs = ((Get-Date) - $script:start).TotalSeconds
        '  {0} files checked, {1} sent ({2:N2} GB, {3:N1} MB/s)' -f $script:checked, $script:sent, ($script:bytes / 1GB), ($script:bytes / 1MB / [math]::Max($secs, 1))
    }
    foreach ($d in Get-ChildItem -LiteralPath $localDir -Directory) {
        Mirror $d.FullName (Ensure-Folder $devFolder $d.Name)
    }
}

$target = $store.GetFolder
foreach ($part in ($Dest -split '[\\/]' | Where-Object { $_ })) { $target = Ensure-Folder $target $part }
$src = (Resolve-Path -LiteralPath $Source).Path
"mirroring $src -> ${Device}:\$Storage\$Dest"
Mirror $src $target
'done: {0} files checked, {1} sent ({2:N2} GB)' -f $script:checked, $script:sent, ($script:bytes / 1GB)
