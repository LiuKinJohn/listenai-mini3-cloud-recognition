param([Parameter(Mandatory=$true)][Uri]$BaseUrl, [string]$ApprovedImage)
$ErrorActionPreference = 'Stop'
if ($BaseUrl.Scheme -ne 'https' -or $BaseUrl.Host -notmatch '\.fcapp\.run$' -or $BaseUrl.UserInfo -or $BaseUrl.Query -or $BaseUrl.Fragment) { throw 'Expected an HTTPS fcapp.run URL without credentials or query parameters.' }
Add-Type -AssemblyName System.Net.Http
function Read-Token($name) {
    $encrypted = (Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot $name)).Trim()
    $secure = ConvertTo-SecureString $encrypted
    $ptr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
    try { return [Runtime.InteropServices.Marshal]::PtrToStringBSTR($ptr) } finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($ptr) }
}
$client = [System.Net.Http.HttpClient]::new()
$client.Timeout = [TimeSpan]::FromSeconds(60)
try {
    $health = [Uri]::new($BaseUrl, '/health')
    $unauth = $client.GetAsync($health).GetAwaiter().GetResult()
    Write-Output ('Unauthenticated health HTTP: ' + [int]$unauth.StatusCode)
    $gateway = Read-Token 'gateway-token.dpapi.txt'
    $device = Read-Token 'device-token-replacement.dpapi.txt'
    $client.DefaultRequestHeaders.Authorization = [System.Net.Http.Headers.AuthenticationHeaderValue]::new('Bearer', $gateway)
    $auth = $client.GetAsync($health).GetAwaiter().GetResult()
    Write-Output ('Authenticated health HTTP: ' + [int]$auth.StatusCode)
    Write-Output ('Health response length: ' + $auth.Content.Headers.ContentLength + '; chunked: ' + $auth.Headers.TransferEncodingChunked)
    $invalid = [System.Net.Http.ByteArrayContent]::new([byte[]](1,2,3,4))
    $invalid.Headers.ContentType = [System.Net.Http.Headers.MediaTypeHeaderValue]::new('image/jpeg')
    $request = [System.Net.Http.HttpRequestMessage]::new([System.Net.Http.HttpMethod]::Post, [Uri]::new($BaseUrl, '/recognize'))
    $request.Headers.Add('X-Device-Token', $device)
    $request.Content = $invalid
    $response = $client.SendAsync($request).GetAwaiter().GetResult()
    Write-Output ('Authenticated invalid-image HTTP: ' + [int]$response.StatusCode)
    if ($ApprovedImage) {
        $bytes = [System.IO.File]::ReadAllBytes($ApprovedImage)
        if ($bytes.Length -gt 358400) { throw 'Image exceeds the allowed size.' }
        $content = [System.Net.Http.ByteArrayContent]::new($bytes)
        $content.Headers.ContentType = [System.Net.Http.Headers.MediaTypeHeaderValue]::new('image/jpeg')
        $send = [System.Net.Http.HttpRequestMessage]::new([System.Net.Http.HttpMethod]::Post, [Uri]::new($BaseUrl, '/recognize'))
        $send.Headers.Add('X-Device-Token', $device)
        $send.Content = $content
        $result = $client.SendAsync($send).GetAwaiter().GetResult()
        Write-Output ('Recognition HTTP: ' + [int]$result.StatusCode)
        Write-Output ('Response length: ' + $result.Content.Headers.ContentLength + '; chunked: ' + $result.Headers.TransferEncodingChunked)
        if ($result.IsSuccessStatusCode) {
            $json = $result.Content.ReadAsStringAsync().GetAwaiter().GetResult() | ConvertFrom-Json
            Write-Output ('Cloud type: ' + $json.type + '; subjective confidence: ' + $json.confidence_description)
        } else {
            try {
                $json = $result.Content.ReadAsStringAsync().GetAwaiter().GetResult() | ConvertFrom-Json
                if ($json.error -in @('model_unavailable','recognition_failed','not_configured','unauthorized','invalid_image')) {
                    Write-Output ('Error category: ' + $json.error + '; upstream HTTP: ' + $json.upstream_status)
                }
            } catch { Write-Output 'Error response is not structured JSON.' }
        }
    }
} catch { Write-Output ('Test failed: ' + $_.Exception.GetType().Name); exit 1 } finally { $client.Dispose(); $gateway = $null; $device = $null }
