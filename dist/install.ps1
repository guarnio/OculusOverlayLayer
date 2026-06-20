$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$json = Join-Path $here "XrApiLayer_OculusOverlayLayer.json"
$key  = "HKCU:\Software\Khronos\OpenXR\1\ApiLayers\Implicit"
New-Item -Path $key -Force | Out-Null
New-ItemProperty -Path $key -Name $json -PropertyType DWord -Value 0 -Force | Out-Null
Write-Host "Registered OpenXR layer: $json"