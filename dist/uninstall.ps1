$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$json = Join-Path $here "XrApiLayer_OculusOverlayLayer.json"
$key  = "HKCU:\Software\Khronos\OpenXR\1\ApiLayers\Implicit"
Remove-ItemProperty -Path $key -Name $json -ErrorAction SilentlyContinue
Write-Host "Unregistered."