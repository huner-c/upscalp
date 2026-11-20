$ErrorActionPreference = 'Stop'
# Download only the official SDK headers and license as build inputs. This does
# not install ADLX, run upstream scripts, or change the AMD driver/settings.
$revision = '32b5a740d42295c5dfe9026b9f52683da0f3af91'
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$buildRoot = [System.IO.Path]::GetFullPath((Join-Path $repoRoot 'build'))
$sdkRoot = [System.IO.Path]::GetFullPath((Join-Path $buildRoot '_deps\adlx'))
if (!$sdkRoot.StartsWith($buildRoot + [System.IO.Path]::DirectorySeparatorChar, [System.StringComparison]::OrdinalIgnoreCase)) { throw 'SDK path escaped the build directory.' }
$headers = @{Accept='application/vnd.github+json'}
$tree = Invoke-RestMethod -Uri "https://api.github.com/repos/GPUOpen-LibrariesAndSDKs/ADLX/git/trees/${revision}?recursive=1" -Headers $headers -TimeoutSec 30
$paths = @($tree.tree | Where-Object { $_.type -eq 'blob' -and ($_.path -match '^SDK/Include/[A-Za-z0-9_]+\.h$' -or $_.path -eq 'ADLX SDK License Agreement.pdf') } | Select-Object -ExpandProperty path)
if ($paths.Count -lt 5 -or $paths.Count -gt 100) { throw 'Unexpected ADLX header list.' }
$files = @()
foreach ($path in $paths) {
    $destination = [System.IO.Path]::GetFullPath((Join-Path $sdkRoot ($path.Replace('/',[System.IO.Path]::DirectorySeparatorChar))))
    if (!$destination.StartsWith($sdkRoot + [System.IO.Path]::DirectorySeparatorChar,[System.StringComparison]::OrdinalIgnoreCase)) { throw 'SDK file path escaped its directory.' }
    New-Item -ItemType Directory -Path (Split-Path $destination) -Force | Out-Null
    if (!(Test-Path -LiteralPath $destination)) {
        $escapedPath = ($path.Split('/') | ForEach-Object { [System.Uri]::EscapeDataString($_) }) -join '/'
        Invoke-WebRequest -Uri "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/ADLX/$revision/$escapedPath" -OutFile $destination -TimeoutSec 30
    }
    $files += [pscustomobject]@{Path=$path;Sha256=(Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash}
}
[pscustomobject]@{Repository='https://github.com/GPUOpen-LibrariesAndSDKs/ADLX';Commit=$revision;Files=$files} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $sdkRoot 'provenance.json') -Encoding utf8
Write-Output "Downloaded $($files.Count) official SDK headers/license files into build\_deps\adlx."
