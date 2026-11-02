# phoenix_uia.ps1 - drive the Phoenix launcher's UI without eyes on the screen.
#
#   -Dump          print the launcher's control tree (types and names) as text
#   -Press <name>  invoke a control matching <name>: exact name first, then Buttons by
#                  substring, then anything by substring (so "PLAY" does not hit a ToolTip
#                  that merely begins with the word "Play")
#   -ProcId <n>    restrict to that process - the launcher's pid
#
# Used to press PLAY for unattended launcher runs: the launcher verifies the install and
# re-downloads FFXiMain.dll, which is exactly the path the stand-in has to survive.
#
# Uses TreeWalker rather than FindAll: PowerShell 7 does not expose the UIAutomation
# Condition types (TrueCondition et al.) that FindAll wants.

param(
    [switch] $Dump,
    [string] $Press,
    [int] $ProcId = 0,
    [string] $WindowName = 'Phoenix'
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$AE = [System.Windows.Automation.AutomationElement]
$walker = [System.Windows.Automation.TreeWalker]::RawViewWalker

function Get-Children($el) {
    $out = @()
    $child = $walker.GetFirstChild($el)
    while ($null -ne $child) {
        $out += $child
        $child = $walker.GetNextSibling($child)
    }
    return $out
}

function Get-Info($el) {
    try {
        [pscustomobject]@{
            Name  = $el.Current.Name
            Type  = $el.Current.ControlType.ProgrammaticName -replace '^ControlType\.', ''
            Class = $el.Current.ClassName
            Pid   = $el.Current.ProcessId
        }
    } catch { $null }
}

$root = $AE::RootElement
$tops = @(Get-Children $root)

$win = $null
foreach ($w in $tops) {
    $i = Get-Info $w
    if (-not $i) { continue }
    if ($ProcId -ne 0 -and $i.Pid -ne $ProcId) { continue }
    if ($i.Name -like "*$WindowName*" -or $i.Class -match 'Chrome_WidgetWin|Tauri') {
        $win = $w; $winInfo = $i; break
    }
}

if (-not $win) {
    Write-Host "no launcher window (ProcId=$ProcId name~'$WindowName'). Top-level windows:"
    foreach ($w in $tops) {
        $i = Get-Info $w
        if ($i) { Write-Host ("  pid={0} type={1} class='{2}' name='{3}'" -f $i.Pid, $i.Type, $i.Class, $i.Name) }
    }
    exit 2
}
Write-Host ("window: name='{0}' class='{1}' pid={2}" -f $winInfo.Name, $winInfo.Class, $winInfo.Pid)

$flat = New-Object System.Collections.Generic.List[object]
function Walk($el, $depth) {
    if ($depth -gt 25) { return }
    foreach ($c in (Get-Children $el)) {
        $i = Get-Info $c
        if ($i) { $flat.Add([pscustomobject]@{ Depth = $depth; Info = $i; El = $c }) }
        Walk $c ($depth + 1)
    }
}
Walk $win 0
Write-Host "controls walked: $($flat.Count)"

function Show-Controls {
    foreach ($e in $flat) {
        $t = $e.Info.Type
        $n = $e.Info.Name
        if ($n -or $t -in 'Button', 'Edit', 'Hyperlink', 'MenuItem', 'TabItem', 'CheckBox', 'ComboBox') {
            Write-Host ("  {0}{1} '{2}'" -f (' ' * $e.Depth), $t, $n)
        }
    }
}

if ($Dump) { Show-Controls; exit 0 }

if ($Press) {
    $named = @($flat | Where-Object { $_.Info.Name })
    $cands = @($named | Where-Object { $_.Info.Name -eq $Press })
    if ($cands.Count -eq 0) {
        $cands = @($named | Where-Object { $_.Info.Type -eq 'Button' -and $_.Info.Name -like "*$Press*" })
    }
    if ($cands.Count -eq 0) {
        $cands = @($named | Where-Object { $_.Info.Name -like "*$Press*" })
    }
    if ($cands.Count -eq 0) {
        Write-Host "no control matching '$Press'. Controls present:"
        Show-Controls
        exit 4
    }

    foreach ($pair in @(
            @{ P = [System.Windows.Automation.InvokePattern];        M = 'Invoke' },
            @{ P = [System.Windows.Automation.TogglePattern];        M = 'Toggle' },
            @{ P = [System.Windows.Automation.SelectionItemPattern]; M = 'Select' })) {
        foreach ($e in $cands) {
            try {
                $o = $e.El.GetCurrentPattern($pair.P::Pattern)
                $o.($pair.M)()
                Write-Host ("PRESS OK: {0} '{1}' via {2}" -f $e.Info.Type, $e.Info.Name, $pair.P.Name)
                exit 0
            } catch { }
        }
    }
    Write-Host ("matched {0} control(s) for '$Press' but none had an Invoke/Toggle/Select pattern" -f $cands.Count)
    foreach ($e in $cands) { Write-Host ("  candidate: {0} '{1}'" -f $e.Info.Type, $e.Info.Name) }
    exit 3
}
