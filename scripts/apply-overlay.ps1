param([Parameter(Mandatory=$true)][string]$SdkRoot)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$sdk = [IO.Path]::GetFullPath($SdkRoot).TrimEnd('\','/')
$expected = (Get-Content -Raw -LiteralPath (Join-Path $repoRoot 'upstream.json') | ConvertFrom-Json).commit
$actual = & git -C $sdk rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $actual.Trim() -ne $expected) { throw 'SDK revision does not match upstream.json.' }
$changes = & git -C $sdk status --porcelain
if ($LASTEXITCODE -ne 0 -or $changes) { throw 'Use a clean SDK checkout before applying this snapshot.' }
$overlay = Join-Path $repoRoot 'overlay'
Get-ChildItem -LiteralPath $overlay -Recurse -File | ForEach-Object {
    $relative = $_.FullName.Substring($overlay.Length).TrimStart('\','/')
    $target = [IO.Path]::GetFullPath((Join-Path $sdk $relative))
    if (-not $target.StartsWith($sdk + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'Target is outside the SDK.' }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
    Copy-Item -LiteralPath $_.FullName -Destination $target -Force
}
Write-Output 'Overlay applied. Build board arcs_mini3 using the SDK instructions.'
