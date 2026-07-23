# Wraps a .maxpat (plain Max patcher JSON) into a Max for Live .amxd device.
#
# An .amxd is a chunked binary container:
#   [ "ampf" | uint32 LE = 4  | 4-byte device-type marker ]   ("mmmm"=MIDI, "aaaa"=audio, "iiii"=instrument)
#   [ "meta" | uint32 LE = 4  | 00 00 00 00                ]
#   [ "ptch" | uint32 LE = N  | <N bytes of the .maxpat JSON>]
# The device type is ALSO stored inside the JSON as project.amxdtype (the marker as a 32-bit int:
#   "mmmm" = 0x6D6D6D6D = 1836411501). Both must agree.
#
# Usage:  powershell -ExecutionPolicy Bypass -File build_amxd.ps1
param(
    [string]$Src    = "$PSScriptRoot\FibloTempoSync.maxpat",
    [string]$Dst    = "$PSScriptRoot\Fibló Tempo Sync.amxd",
    [string]$Marker = "mmmm"   # MIDI Effect device
)

$payload = [System.IO.File]::ReadAllBytes($Src)
# Strip a UTF-8 BOM if the editor added one (Max JSON must start with '{').
if ($payload.Length -ge 3 -and $payload[0] -eq 0xEF -and $payload[1] -eq 0xBB -and $payload[2] -eq 0xBF) {
    $payload = $payload[3..($payload.Length - 1)]
}
if ([char]$payload[0] -ne '{') { throw "payload does not start with '{' - not a maxpat JSON" }

$A  = [System.Text.Encoding]::ASCII
$ms = New-Object System.IO.MemoryStream
$ms.Write($A.GetBytes("ampf"), 0, 4); $ms.Write([BitConverter]::GetBytes([int32]4), 0, 4); $ms.Write($A.GetBytes($Marker), 0, 4)
$ms.Write($A.GetBytes("meta"), 0, 4); $ms.Write([BitConverter]::GetBytes([int32]4), 0, 4); $ms.Write([byte[]]@(0,0,0,0), 0, 4)
$ms.Write($A.GetBytes("ptch"), 0, 4); $ms.Write([BitConverter]::GetBytes([int32]$payload.Length), 0, 4); $ms.Write($payload, 0, $payload.Length)
[System.IO.File]::WriteAllBytes($Dst, $ms.ToArray())
$ms.Dispose()

Write-Output ("Wrote '$Dst'  (marker='$Marker', payload=$($payload.Length) bytes, total=$(32 + $payload.Length) bytes)")
