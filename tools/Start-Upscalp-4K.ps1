param(
  [switch]$SmokeTest,
  [switch]$DriverOwner,
  [ValidatePattern('^[^"]*$')][string]$OutputDisplay,
  [ValidatePattern('^[a-f0-9]{32}$')][string]$SessionId,
  [int]$OwnerPid,
  [long]$OwnerStartTicks
)

$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot
$sessions = Join-Path $project 'build\iddcx\sessions'

function Set-SessionFile($Directory, $Name, $Value) {
  $temporary = Join-Path $Directory "$Name.tmp"
  Set-Content -LiteralPath $temporary -Value $Value -Encoding UTF8
  Move-Item -LiteralPath $temporary -Destination (Join-Path $Directory $Name)
}

function Get-AmyuniPackage {
  Get-WindowsDriver -Online | Where-Object {
    $_.ProviderName -eq 'Amyuni' -and [IO.Path]::GetFileName($_.OriginalFileName) -eq 'usbmmidd.inf'
  }
}

function Get-AmyuniDevice {
  Get-PnpDevice -Class Display -PresentOnly | Where-Object {
    $_.FriendlyName -eq 'USB Mobile Monitor Virtual Display'
  }
}

function Start-DriverSession($Directory, $Owner) {
  $vendor = Join-Path $project 'build\_deps\amyuni-usbmmidd-v2\files\usbmmidd_v2'
  $devcon = Join-Path $project 'build\_deps\wdk\10.0.26100.6584\c\tools\10.0.26100.0\x64\devcon.exe'
  $pnp = Join-Path $env:SystemRoot 'System32\pnputil.exe'
  $signTool = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe'
  $deviceHelper = Join-Path $vendor 'deviceinstaller64.exe'
  $stopFile = Join-Path $Directory 'stop'
  $installAttempted = $false
  $activationAttempted = $false
  $originalPackages = @()
  $result = 1

  Start-Transcript -LiteralPath (Join-Path $Directory 'driver.log') | Out-Null
  try {
    if ($Owner.HasExited) { throw 'Baslatan pencere kapandi.' }
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $deviceHelper).Hash -ne 'E9BAA02CDAE921ACF0AAE4D8E8C29A4CDF4057AB61F9C60862B7CC439E2753F7') {
      throw 'Amyuni yardimci uygulamasi beklenen dosyayla eslesmiyor.'
    }
    $catalog = Join-Path $vendor 'usbmmIdd.cat'
    $inf = Join-Path $vendor 'usbmmIdd.inf'
    foreach ($file in @($inf, (Join-Path $vendor 'x64\usbmmIdd.dll'))) {
      & $signTool verify /kp /c $catalog $file
      if ($LASTEXITCODE) { throw "Amyuni surucu imzasi dogrulanamadi. $file" }
    }
    $originalPackages = @(Get-AmyuniPackage)
    $originalDevices = @(Get-AmyuniDevice)
    if ($originalPackages.Count -gt 1 -or $originalDevices.Count -gt 1) {
      throw 'Birden fazla Amyuni kurulumu bulundu. Hangi aygitin kullanilacagi dogrulanamadi.'
    }
    if ($originalPackages.Count) {
      $package = $originalPackages[0]
      $store = Split-Path -Parent $package.OriginalFileName
      foreach ($relativePath in 'usbmmIdd.inf','usbmmIdd.cat','x64\usbmmIdd.dll') {
        if ((Get-FileHash -LiteralPath (Join-Path $store $relativePath)).Hash -ne
            (Get-FileHash -LiteralPath (Join-Path $vendor $relativePath)).Hash) {
          throw 'Kurulu Amyuni paketi Upscalp paketinden farkli. Kurulu surucu degistirilmedi.'
        }
      }
      if ($originalDevices.Count) {
        $existing = $originalDevices[0]
        $properties = Get-PnpDeviceProperty -InstanceId $existing.InstanceId `
          -KeyName DEVPKEY_Device_DriverInfPath,DEVPKEY_Device_HardwareIds
        $boundPackage = ($properties | Where-Object KeyName -eq 'DEVPKEY_Device_DriverInfPath').Data
        $hardwareIds = ($properties | Where-Object KeyName -eq 'DEVPKEY_Device_HardwareIds').Data
        if ($boundPackage -ne $package.Driver -or $hardwareIds -notcontains 'usbmmIdd' -or $existing.Status -ne 'OK') {
          throw 'Kurulu Amyuni aygiti hazir degil veya dogrulanan pakete bagli degil.'
        }
      }
      Write-Host "Dogrulanan Amyuni paketi yeniden kullaniliyor ($($package.Driver))."
    } elseif ($originalDevices.Count) {
      throw 'Kurulu Amyuni aygitinin surucu paketi dogrulanamadi.'
    }

    # Enter the exact mode validated by Windows without saving a persistent mode change.
    Add-Type -TypeDefinition @'
    using System;
    using System.Runtime.InteropServices;
    public static class UpscalpSessionDisplay {
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        struct DisplayDevice {
            public int size;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string name;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string description;
            public uint state;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string id;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string key;
        }
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        struct Mode {
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string name;
            public ushort spec, driver, size, extra;
            public uint fields;
            public int x, y;
            public uint orientation, fixedOutput;
            public short color, duplex, yResolution, ttOption, collate;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string form;
            public ushort logPixels;
            public uint bpp, width, height, flags, refresh, icmMethod, icmIntent, mediaType, ditherType;
            public uint reserved1, reserved2, panningWidth, panningHeight;
        }
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        static extern bool EnumDisplayDevices(string name, int index, ref DisplayDevice device, uint flags);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        static extern bool EnumDisplaySettings(string name, int index, ref Mode mode);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        static extern int ChangeDisplaySettingsEx(string name, ref Mode mode, IntPtr window, uint flags, IntPtr data);

        public static string FindActive() {
            for (int i = 0; ; ++i) {
                var device = new DisplayDevice { size = Marshal.SizeOf(typeof(DisplayDevice)) };
                if (!EnumDisplayDevices(null, i, ref device, 0)) return null;
                if ((device.state & 1) != 0 && device.description == "USB Mobile Monitor Virtual Display") return device.name;
            }
        }
        public static string Activate4k(string name) {
            for (int i = 0; ; ++i) {
                var mode = new Mode { size = (ushort)Marshal.SizeOf(typeof(Mode)) };
                if (!EnumDisplaySettings(name, i, ref mode)) throw new InvalidOperationException("No 3840x2160/60 mode reported by Windows.");
                if (mode.width != 3840 || mode.height != 2160 || (mode.refresh != 59 && mode.refresh != 60)) continue;
                mode.fields = 0x00080000 | 0x00100000 | 0x00400000;
                int test = ChangeDisplaySettingsEx(name, ref mode, IntPtr.Zero, 2, IntPtr.Zero);
                if (test != 0) continue;
                int result = ChangeDisplaySettingsEx(name, ref mode, IntPtr.Zero, 0, IntPtr.Zero);
                if (result != 0) throw new InvalidOperationException("4K mode application returned " + result);
                var actual = new Mode { size = (ushort)Marshal.SizeOf(typeof(Mode)) };
                if (!EnumDisplaySettings(name, -1, ref actual) || actual.width != 3840 || actual.height != 2160 || (actual.refresh != 59 && actual.refresh != 60))
                    throw new InvalidOperationException("Active virtual desktop is not 3840x2160.");
                return name + " " + actual.width + "x" + actual.height + " @ " + actual.refresh + "Hz";
            }
        }
        public static bool Is4k60(string name) {
            var mode = new Mode { size = (ushort)Marshal.SizeOf(typeof(Mode)) };
            return EnumDisplaySettings(name, -1, ref mode) && mode.width == 3840 && mode.height == 2160 &&
                   (mode.refresh == 59 || mode.refresh == 60);
        }
    }
'@

    if ($Owner.HasExited -or (Test-Path -LiteralPath $stopFile)) { throw 'Oturum acilmadan kapatildi.' }
    if (!$originalDevices.Count) {
      $devconSignature = Get-AuthenticodeSignature -LiteralPath $devcon
      if ($devconSignature.Status -ne 'Valid' -or $devconSignature.SignerCertificate.Subject -notlike '*O=Microsoft Corporation*') {
        throw 'Microsoft DevCon imzasi dogrulanamadi.'
      }
      $installAttempted = $true
      & $devcon install $inf usbmmIdd
      if ($LASTEXITCODE) { throw "Amyuni surucusu yuklenemedi. DevCon kodu $LASTEXITCODE." }
    }

    $active = [UpscalpSessionDisplay]::FindActive()
    if (!$active) {
      $activationAttempted = $true
      & $deviceHelper enableidd 1
      if ($LASTEXITCODE) { throw "Sanal ekran baglanamadi. Amyuni kodu $LASTEXITCODE." }
    }
    for ($attempt = 0; !$active -and $attempt -lt 30; $attempt++) {
      if ($Owner.HasExited -or (Test-Path -LiteralPath $stopFile)) { throw 'Oturum acilmadan kapatildi.' }
      $active = [UpscalpSessionDisplay]::FindActive()
      if (!$active) { Start-Sleep -Milliseconds 500 }
    }
    if (!$active) { throw 'Windows sanal ekrani etkinlestirmedi. Goruntu ayarlarinda masaustu genisletilmis olmali.' }
    if ($activationAttempted) {
      [UpscalpSessionDisplay]::Activate4k($active)
    } elseif (![UpscalpSessionDisplay]::Is4k60($active)) {
      throw 'Onceden acik Amyuni ekrani 4K/60 Hz modunda degil.'
    }
    Set-SessionFile $Directory 'ready' $active

    while (!$Owner.HasExited -and !(Test-Path -LiteralPath $stopFile)) {
      Start-Sleep -Milliseconds 500
    }
    $result = 0
  } catch {
    Write-Host $_.Exception.Message
    Set-SessionFile $Directory 'error' $_.Exception.Message
  } finally {
    try {
      $deactivateCode = 0
      if ($activationAttempted) {
        & $deviceHelper enableidd 0
        $deactivateCode = $LASTEXITCODE
      }
      if ($installAttempted) {
        foreach ($device in @(Get-AmyuniDevice)) {
          & $pnp /remove-device $device.InstanceId
          if ($LASTEXITCODE) { throw "Gecici Amyuni aygiti kaldirilamadi. $($device.InstanceId)" }
        }
        foreach ($package in @(Get-AmyuniPackage | Where-Object { $_.Driver -notin $originalPackages.Driver })) {
          & $pnp /delete-driver $package.Driver
          if ($LASTEXITCODE) { throw "Gecici surucu paketi kaldirilamadi. $($package.Driver)" }
        }
        if (@(Get-AmyuniDevice).Count -or
            @(Get-AmyuniPackage | Where-Object { $_.Driver -notin $originalPackages.Driver }).Count) {
          throw 'Gecici Amyuni surucusu kaldirma sonrasi hala mevcut.'
        }
      }
      if ($deactivateCode -and !$installAttempted) {
        throw "Bu oturumun sanal ekrani kapatilamadi. Amyuni kodu $deactivateCode."
      }
    } catch {
      Write-Host $_.Exception.Message
      Set-SessionFile $Directory 'cleanup-error' $_.Exception.Message
      $result = 1
    } finally {
      Stop-Transcript | Out-Null
    }
  }
  exit $result
}

if ($DriverOwner) {
  if (!$SessionId -or !$OwnerPid -or !$OwnerStartTicks) { throw 'Oturum bilgileri eksik.' }
  $owner = Get-Process -Id $OwnerPid
  if ($owner.StartTime.ToUniversalTime().Ticks -ne $OwnerStartTicks) { throw 'Baslatan pencere degisti.' }
  Start-DriverSession (Join-Path $sessions $SessionId) $owner
}

$mutex = [Threading.Mutex]::new($false, 'Local\Upscalp.4KSession')
$locked = $false
$driver = $null
$exitCode = 1
try {
  try { $locked = $mutex.WaitOne(0) } catch [Threading.AbandonedMutexException] { $locked = $true }
  if (!$locked) { throw 'Bir Upscalp 4K oturumu zaten acik.' }
  $app = Join-Path $project 'out\Upscalp-4K.exe'
  if (!(Test-Path -LiteralPath $app)) { $app = Join-Path $project 'out\Upscalp.exe' }
  if (!(Test-Path -LiteralPath $app)) { throw 'out\Upscalp.exe bulunamadi.' }

  $SessionId = [guid]::NewGuid().ToString('N')
  $directory = Join-Path $sessions $SessionId
  $null = New-Item -Path $directory -ItemType Directory -Force
  $winPs = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
  $ownerTicks = (Get-Process -Id $PID).StartTime.ToUniversalTime().Ticks
  $ownerArgs = '-NoProfile -File "{0}" -DriverOwner -SessionId {1} -OwnerPid {2} -OwnerStartTicks {3}' -f $PSCommandPath,$SessionId,$PID,$ownerTicks
  Write-Host 'Windows yonetici onayindan sonra 4K ekran hazirlanacak.'
  $driver = Start-Process -FilePath $winPs -ArgumentList $ownerArgs -Verb RunAs -WindowStyle Hidden -PassThru

  $ready = Join-Path $directory 'ready'
  $errorFile = Join-Path $directory 'error'
  $deadline = [Diagnostics.Stopwatch]::StartNew()
  while (!(Test-Path -LiteralPath $ready)) {
    if (Test-Path -LiteralPath $errorFile) { throw (Get-Content -Raw -LiteralPath $errorFile).Trim() }
    if ($driver.HasExited) { throw 'Sanal ekran yardimcisi erken kapandi.' }
    if ($deadline.Elapsed.TotalSeconds -ge 60) { throw 'Sanal ekran 60 saniyede hazirlanamadi.' }
    Start-Sleep -Milliseconds 250
  }
  $source = (Get-Content -Raw -LiteralPath $ready).Trim()
  if ($source -notmatch '^\\\\\.\\DISPLAY[0-9]+$') { throw 'Windows sanal ekran kimligi gecersiz.' }

  Write-Host "4K ekran hazir ($source). Oyun bitince Upscalp penceresini kapat."
  $appArgs = '--source-display "{0}"' -f $source
  if ($OutputDisplay) { $appArgs += ' --output-display "{0}"' -f $OutputDisplay }
  if ($SmokeTest) {
    $report = Join-Path $directory 'mirror-test.json'
    $appArgs += ' --monitor-smoke-test --report "{0}"' -f $report
  } else {
    $appArgs += ' --replace'
  }
  $running = Start-Process -FilePath $app -ArgumentList $appArgs -PassThru
  $running.WaitForExit()
  if ($running.ExitCode) { throw "Upscalp hata kodu $($running.ExitCode) ile kapandi." }
  if ($SmokeTest) {
    $measurement = Get-Content -Raw -LiteralPath $report | ConvertFrom-Json
    if (!$measurement.passed -or $measurement.sourceWidth -ne 3840 -or $measurement.sourceHeight -ne 2160 -or $measurement.submittedFrames -lt 60) {
      throw '4K goruntu testi dogrulanamadi.'
    }
    Write-Host "4K goruntu testi gecti. Rapor $report"
  }
  $exitCode = 0
} catch {
  Write-Host $_.Exception.Message
  if ($directory) { Write-Host "Oturum kayitlari $directory" }
} finally {
  if ($driver) {
    Set-Content -LiteralPath (Join-Path $directory 'stop') -Value '' -Encoding ASCII
    if (!$driver.WaitForExit(45000)) {
      Write-Host "Surucu kaldirma islemi beklenenden uzun suruyor. Kayit $directory"
      $exitCode = 1
    } elseif ($driver.ExitCode -and $exitCode -eq 0) {
      Write-Host "Surucu kullanimi tamamlanamadi. Kayit $directory"
      $exitCode = 1
    }
  }
  if ($locked) { $mutex.ReleaseMutex() }
  $mutex.Dispose()
}
exit $exitCode
