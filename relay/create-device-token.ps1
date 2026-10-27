param([switch]$Replacement, [switch]$Gateway)
$ErrorActionPreference = 'Stop'
$filename = if ($Gateway) { 'gateway-token.dpapi.txt' } elseif ($Replacement) { 'device-token-replacement.dpapi.txt' } else { 'device-token.dpapi.txt' }
$path = Join-Path $PSScriptRoot $filename
if (Test-Path -LiteralPath $path) { throw 'A protected token already exists. Do not overwrite it.' }
$bytes = New-Object byte[] 32
$rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
try { $rng.GetBytes($bytes) } finally { $rng.Dispose() }
$token = if ($Gateway) { -join ($bytes | ForEach-Object { $_.ToString('x2') }) } else { [Convert]::ToBase64String($bytes).TrimEnd('=').Replace('+','-').Replace('/','_') }
$secure = ConvertTo-SecureString $token -AsPlainText -Force
ConvertFrom-SecureString $secure | Set-Content -LiteralPath $path -Encoding ASCII
Set-Clipboard -Value $token
[Array]::Clear($bytes, 0, $bytes.Length)
Write-Host 'Token copied to clipboard and saved encrypted for this Windows user.'
Write-Host 'Paste it into the intended token field; do not paste it into chat.'
$token = $null
