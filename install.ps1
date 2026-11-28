$ErrorActionPreference = 'Stop'
if ([Runtime.InteropServices.RuntimeInformation]::OSArchitecture -ne 'X64') {
    throw 'The Windows release requires x86-64.'
}

$asset = 'si-windows-x86_64.zip'
$url = 'https://github.com/dyne/sixel-viz/releases/latest/download'
$prefix = if ($env:SI_PREFIX) { $env:SI_PREFIX } else { Join-Path $env:LOCALAPPDATA 'sixel-viz' }
$temp = Join-Path $prefix ('.si-' + [guid]::NewGuid())
$zip = Join-Path $temp $asset
New-Item -ItemType Directory -Path $temp -Force | Out-Null
try {
    $checksums = Join-Path $temp 'SHA256SUMS.txt'
    Invoke-WebRequest -UseBasicParsing -TimeoutSec 30 "$url/SHA256SUMS.txt" -OutFile $checksums
    Invoke-WebRequest -UseBasicParsing -TimeoutSec 180 "$url/$asset" -OutFile $zip
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $zip).Hash.ToLowerInvariant()
    if ((Get-Content -LiteralPath $checksums -Raw) -notmatch "(?m)^$hash[ \t]+$([regex]::Escape($asset))\r?$") {
        throw "SHA-256 verification failed for $asset."
    }

    $bundle = Join-Path $prefix $hash
    $exe = Join-Path $bundle 'si.exe'
    if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
        $package = Join-Path $temp 'package'
        Expand-Archive -LiteralPath $zip -DestinationPath $package
        if (-not (Test-Path -LiteralPath (Join-Path $package 'si.exe') -PathType Leaf)) {
            throw "Missing si.exe in $asset."
        }
        if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
            if (Test-Path -LiteralPath $bundle) { throw "Incomplete installation at $bundle." }
            Move-Item -LiteralPath $package -Destination $bundle
        }
    }
    Write-Output "Installed $exe"
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force
}
