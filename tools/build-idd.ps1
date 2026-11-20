param(
  [string]$WdkRoot,
  [string]$VsRoot
)

$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot
$build = Join-Path $project 'build\iddcx'
$package = Join-Path $project 'out\iddcx'
if (!$WdkRoot) { $WdkRoot = Join-Path $project 'build\_deps\wdk\10.0.26100.6584\c' }
$WdkRoot = (Resolve-Path -LiteralPath $WdkRoot).Path

if (!$VsRoot) {
  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  if (Test-Path -LiteralPath $vswhere) {
    $VsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  }
}
if (!$VsRoot) { throw 'Set -VsRoot to a Visual Studio 2022 installation with x64 C++ build tools.' }

$cmake = Join-Path $VsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (!(Test-Path -LiteralPath $cmake)) { $cmake = (Get-Command cmake -ErrorAction Stop).Source }
$inf2cat = Join-Path $WdkRoot 'bin\10.0.26100.0\x86\Inf2Cat.exe'
if (!(Test-Path -LiteralPath $inf2cat)) { throw 'Inf2Cat was not found under -WdkRoot.' }
$infVerif = Join-Path $WdkRoot 'tools\10.0.26100.0\x64\infverif.exe'
if (!(Test-Path -LiteralPath $infVerif)) { throw 'InfVerif was not found under -WdkRoot.' }

& $cmake -S $project -B $build -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_GENERATOR_INSTANCE=$VsRoot" `
  '-DCMAKE_SYSTEM_VERSION=10.0.26100.0' "-DUPSCALP_WDK_ROOT=$WdkRoot"
if ($LASTEXITCODE) { throw 'IddCx CMake configuration failed.' }
& $cmake --build $build --config Release --target UpscalpIdd UpscalpIddDevice --parallel 1
if ($LASTEXITCODE) { throw 'IddCx compilation failed.' }

# Generate into the build tree so a failed validation cannot publish a partial package.
$staging = Join-Path $build 'package'
$null = New-Item -ItemType Directory -Path $staging -Force
$source = Join-Path $build 'driver\Release'
Copy-Item -LiteralPath (Join-Path $project 'driver\UpscalpIdd.inf') -Destination $staging -Force
Copy-Item -LiteralPath (Join-Path $source 'UpscalpIdd.dll') -Destination $staging -Force
& $infVerif /w (Join-Path $staging 'UpscalpIdd.inf')
if ($LASTEXITCODE) { throw 'InfVerif Windows Driver validation failed.' }
& $inf2cat "/driver:$staging" '/os:10_CO_X64,10_NI_X64,10_GE_X64'
if ($LASTEXITCODE) { throw 'Inf2Cat driver package validation failed.' }

$null = New-Item -ItemType Directory -Path $package -Force
Copy-Item -LiteralPath (Join-Path $staging 'UpscalpIdd.inf'), (Join-Path $staging 'UpscalpIdd.dll'), `
  (Join-Path $staging 'UpscalpIdd.cat'), (Join-Path $source 'UpscalpIddDevice.exe') -Destination $package -Force
Write-Output $package
