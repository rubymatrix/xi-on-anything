# Builds the app, signs it, and installs it on an Xbox in Developer Mode through Device Portal.
#
#   powershell -File xbox\tools\deploy-xbox.ps1 -Xbox <IP address> [-Configuration Release]
#
# Build the game first: python tools\build.py uwp-lib.
# Needs Device Portal on (Dev Home > Remote Access Settings); asks for its user name and password.
# The package is signed with a self-signed test certificate made on first use (build\xbox\XIonXbox.pfx,
# not in git): dev mode installs test-signed packages, and nothing here is published.
#   powershell -File xbox\tools\deploy-xbox.ps1 -PackageOnly
#
# -PackageOnly stops once the package is signed (build\xbox\XIonXbox-xbox.msix, with build\xbox\XIonXbox.cer):
# for installing it from another computer on the Xbox's network, in Device Portal's page
# (https://<Xbox>:11443, Add), when this one cannot reach the console.
param(
    [string]$Xbox,
    [switch]$PackageOnly,
    [string]$Configuration = 'Release',
    [System.Management.Automation.PSCredential]$Credential
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot   # xbox\
$build = Join-Path (Split-Path -Parent $root) 'build\xbox'
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -prerelease -property installationPath
$msbuild = Join-Path $vs 'MSBuild\Current\Bin\amd64\MSBuild.exe'
$sdkbin = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\10.*" -Directory | Sort-Object Name -Descending |
    ForEach-Object { Join-Path $_.FullName 'x64' } | Where-Object { Test-Path (Join-Path $_ 'signtool.exe') } | Select-Object -First 1

# --- the test certificate: the manifest's publisher, for code signing, made once ------------------------
$pfx = Join-Path $build 'XIonXbox.pfx'
$cer = Join-Path $build 'XIonXbox.cer'
if (-not (Test-Path $pfx)) {
    New-Item -ItemType Directory -Force $build | Out-Null
    $cert = New-SelfSignedCertificate -Type Custom -Subject 'CN=XIonXbox' -KeyUsage DigitalSignature -FriendlyName 'XI on Xbox (test signing)' `
        -CertStoreLocation 'Cert:\CurrentUser\My' -NotAfter (Get-Date).AddYears(5) `
        -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3', '2.5.29.19={text}')
    Export-PfxCertificate -Cert $cert -FilePath $pfx -Password (New-Object System.Security.SecureString) | Out-Null
    Export-Certificate -Cert $cert -FilePath $cer | Out-Null
    Remove-Item "Cert:\CurrentUser\My\$($cert.Thumbprint)"   # the .pfx is all that signing needs
    "made the test certificate: $pfx"
}

# --- build, and sign the package ---------------------------------------------------------------------------
& $msbuild (Join-Path $root 'app\XIonXbox.vcxproj') "-p:Configuration=$Configuration" -p:Platform=x64 -m -nologo -v:m
if ($LASTEXITCODE) { throw "build failed" }
$package = Get-ChildItem (Join-Path $build 'packages') -Recurse -Filter 'XIonXbox_*_x64.msix' |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
$signed = Join-Path $build 'XIonXbox-xbox.msix'
Copy-Item $package.FullName $signed -Force
& (Join-Path $sdkbin 'signtool.exe') sign /fd SHA256 /f $pfx $signed | Out-Null   # the .pfx has no password
if ($LASTEXITCODE) { throw "signing failed" }
"signed $signed"
if ($PackageOnly) {
    "for Device Portal's Add: $signed and its certificate $cer"
    return
}
if (-not $Xbox) { throw "-Xbox <IP address>, or -PackageOnly" }

# --- install through Device Portal --------------------------------------------------------------------------
if (-not $Credential) { $Credential = Get-Credential -Message "Device Portal on $Xbox (Dev Home > Remote Access Settings)" }
# Device Portal's HTTPS certificate is the console's own, self-signed: accepted for this address only
Add-Type @"
using System.Net; using System.Net.Security; using System.Security.Cryptography.X509Certificates;
public static class XboxTrust {
    static string host;
    static bool Check(object s, X509Certificate c, X509Chain ch, SslPolicyErrors e) {
        var r = s as HttpWebRequest; return e == SslPolicyErrors.None || (r != null && r.RequestUri.Host == host);
    }
    public static void Install(string h) { host = h; ServicePointManager.ServerCertificateValidationCallback = Check; }
}
"@
[XboxTrust]::Install($Xbox)
[System.Net.ServicePointManager]::SecurityProtocol = [System.Net.SecurityProtocolType]::Tls12

$portal = "https://${Xbox}:11443"
$session = New-Object Microsoft.PowerShell.Commands.WebRequestSession
# a first request sets the CSRF cookie that writes must echo
Invoke-WebRequest "$portal/api/os/info" -Credential $Credential -WebSession $session -UseBasicParsing | Out-Null
$csrf = ($session.Cookies.GetCookies($portal) | Where-Object Name -eq 'CSRF-Token').Value

$boundary = [Guid]::NewGuid().ToString()
$body = New-Object System.IO.MemoryStream
$w = New-Object System.IO.StreamWriter($body)
function Part($name, $path) {
    $w.Write("--$boundary`r`nContent-Disposition: form-data; name=`"$name`"; filename=`"$(Split-Path -Leaf $path)`"`r`n" +
        "Content-Type: application/octet-stream`r`n`r`n"); $w.Flush()
    $bytes = [IO.File]::ReadAllBytes($path); $body.Write($bytes, 0, $bytes.Length)
    $w.Write("`r`n"); $w.Flush()
}
Part (Split-Path -Leaf $signed) $signed
Part (Split-Path -Leaf $cer) $cer
$w.Write("--$boundary--`r`n"); $w.Flush()
"uploading $([math]::Round($body.Length / 1MB, 1)) MB to $Xbox"
Invoke-WebRequest "$portal/api/app/packagemanager/package?package=$(Split-Path -Leaf $signed)" -Method Post -Credential $Credential `
    -WebSession $session -Headers @{ 'X-CSRF-Token' = $csrf } -ContentType "multipart/form-data; boundary=$boundary" `
    -Body $body.ToArray() -UseBasicParsing | Out-Null

# the install runs on the console; its state answers 204 until it is done
do {
    Start-Sleep -Seconds 2
    $r = Invoke-WebRequest "$portal/api/app/packagemanager/state" -Credential $Credential -WebSession $session -UseBasicParsing
} while ($r.StatusCode -eq 204)
$state = $r.Content | ConvertFrom-Json
if ($state.Success -eq $false) { throw "install failed on the Xbox: $($state.Reason) (code $($state.Code))" }
"installed on $Xbox. In Dev Home, select XI on Xbox > View details, and set its type to Game."
