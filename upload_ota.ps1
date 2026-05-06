param(
  [Parameter(Mandatory = $true)]
  [string]$Ip,

  [string]$Password = ""
)

$ErrorActionPreference = "Stop"

$cli = "C:\Users\juane.DESKTOP-NL44BQ6\AppData\Local\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
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
