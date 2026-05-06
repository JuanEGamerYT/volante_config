param(
  [Parameter(Mandatory = $true)]
  [string]$Ip,

  [string]$Password = "",

  [string]$ArduinoCli = ""
)

$ErrorActionPreference = "Stop"

function Find-ArduinoCli {
  if ($ArduinoCli -ne "" -and (Test-Path $ArduinoCli)) {
    return $ArduinoCli
  }

  $cmd = Get-Command "arduino-cli" -ErrorAction SilentlyContinue
  if ($null -ne $cmd) {
    return $cmd.Source
  }

  $candidates = @()
  if ($env:LOCALAPPDATA) {
    $candidates += Join-Path $env:LOCALAPPDATA "Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
  }
  if ($env:ProgramFiles) {
    $candidates += Join-Path $env:ProgramFiles "Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
  }
  $programFilesX86 = [Environment]::GetEnvironmentVariable("ProgramFiles(x86)")
  if ($programFilesX86) {
    $candidates += Join-Path $programFilesX86 "Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
  }

  foreach ($candidate in $candidates) {
    if ($candidate -and (Test-Path $candidate)) {
      return $candidate
    }
  }

  throw "No encontre arduino-cli. Instala Arduino IDE o agrega arduino-cli al PATH."
}

$cli = Find-ArduinoCli
$fqbn = "esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc,UploadMode=cdc"
$sketchDir = Join-Path $PSScriptRoot "esp32s3_bridge"
$buildDir = Join-Path $PSScriptRoot "build\esp32s3_bridge_ota"

New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

& $cli compile --fqbn $fqbn --output-dir $buildDir $sketchDir
if ($LASTEXITCODE -ne 0) {
  exit $LASTEXITCODE
}

$uploadArgs = @(
  "upload",
  "-p", $Ip,
  "--protocol", "network",
  "--fqbn", $fqbn,
  "--input-dir", $buildDir
)

if ($Password -ne "") {
  $uploadArgs += @("--upload-field", "password=$Password")
}

$uploadArgs += $sketchDir

& $cli @uploadArgs
exit $LASTEXITCODE
