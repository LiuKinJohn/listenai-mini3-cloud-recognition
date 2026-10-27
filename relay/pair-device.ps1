param([Parameter(Mandatory=$true)][string]$Serial, [string]$Adb='adb')
$ErrorActionPreference = 'Stop'
if ($Serial -notmatch '^[A-Za-z0-9:._-]+$') { throw 'Invalid device serial.' }
function Read-Token($name) {
    $text = (Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot $name)).Trim()
    $secure = ConvertTo-SecureString $text
    $ptr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
    try { return [Runtime.InteropServices.Marshal]::PtrToStringBSTR($ptr) }
    finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($ptr) }
}
$gateway = Read-Token 'gateway-token.dpapi.txt'
$device = Read-Token 'device-token-replacement.dpapi.txt'
if ($gateway -notmatch '^[0-9a-f]{64}$' -or $device -notmatch '^[A-Za-z0-9_-]{43}$') { throw 'Unexpected token format.' }
$info = [Diagnostics.ProcessStartInfo]::new()
$info.FileName = $adb
$info.Arguments = '-s ' + $serial + ' shell'
$info.UseShellExecute = $false
$info.CreateNoWindow = $true
$info.RedirectStandardInput = $true
$info.RedirectStandardOutput = $true
$info.RedirectStandardError = $true
$process = [Diagnostics.Process]::new()
$process.StartInfo = $info
try {
    if (-not $process.Start()) { throw 'Unable to start adb.' }
    $outTask = $process.StandardOutput.ReadToEndAsync()
    $errTask = $process.StandardError.ReadToEndAsync()
    $process.StandardInput.WriteLine('kv set string nano.cloud.gateway ' + $gateway)
    Start-Sleep -Seconds 1
    $process.StandardInput.WriteLine('kv set string nano.cloud.device ' + $device)
    Start-Sleep -Seconds 2
    $process.StandardInput.Close()
    if (-not $process.WaitForExit(5000)) { $process.Kill(); $process.WaitForExit() }
    $output = $outTask.GetAwaiter().GetResult()
    if (-not ($output.Contains('flash set nano.cloud.gateway:') -and $output.Contains('flash set nano.cloud.device:'))) { throw 'Could not confirm both configuration writes.' }
    if ($output -match 'flash set nano\.cloud\.(gateway|device):[^\r\n]*failed') { throw 'A configuration write failed.' }
    if (($output | Select-String -Pattern 'flash set nano\.cloud\.(gateway|device):[^\r\n]*success' -AllMatches).Matches.Count -ne 2) { throw 'Could not confirm both successful writes.' }
    Write-Output 'Both device access tokens were saved.'
} finally {
    if ($process -and -not $process.HasExited) { $process.Kill() }
    $process.Dispose()
    $gateway = $null; $device = $null; $output = $null
}
