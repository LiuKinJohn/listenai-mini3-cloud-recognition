$ErrorActionPreference = 'Stop'
$path = Join-Path $PSScriptRoot 'device-token-replacement.dpapi.txt'
$encrypted = (Get-Content -Raw -LiteralPath $path).Trim()
$secure = ConvertTo-SecureString $encrypted
$ptr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
try { Set-Clipboard -Value ([Runtime.InteropServices.Marshal]::PtrToStringBSTR($ptr)) }
finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($ptr) }
Write-Host 'Device token copied to clipboard. Paste it only into the DeviceToken field.'
