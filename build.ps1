param([ValidateSet('Debug','Release')][string]$Configuration = 'Release', [switch]$Test)
$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio 2022 C++ Build Tools ve Windows SDK gerekli.' }
$vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vsRoot) { throw 'Visual Studio C++ toolchain bulunamadi.' }
$cmakePath = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (!(Test-Path -LiteralPath $cmakePath)) { $cmakePath = (Get-Command cmake -ErrorAction Stop).Source }
$buildPath = Join-Path $PSScriptRoot 'build'
& $cmakePath -S $PSScriptRoot -B $buildPath -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_GENERATOR_INSTANCE=$vsRoot"
if ($LASTEXITCODE -ne 0) { throw 'CMake configure basarisiz.' }
& $cmakePath --build $buildPath --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { throw 'Derleme basarisiz.' }
if ($Test) {
  $ctestPath = Join-Path (Split-Path $cmakePath) 'ctest.exe'
  & $ctestPath --test-dir $buildPath -C $Configuration --output-on-failure
  if ($LASTEXITCODE -ne 0) { throw 'GPU pipeline testi basarisiz.' }
}
Write-Output (Join-Path $buildPath "$Configuration\Upscalp.exe")
