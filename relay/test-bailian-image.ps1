param(
    [Parameter(Mandatory = $true)]
    [string]$ImagePath
)

$ErrorActionPreference = 'Stop'
$resolvedImage = (Resolve-Path -LiteralPath $ImagePath).Path
$imageBytes = [System.IO.File]::ReadAllBytes($resolvedImage)
if ($imageBytes.Length -lt 4 -or $imageBytes[0] -ne 0xff -or $imageBytes[1] -ne 0xd8) {
    throw 'Please choose a JPEG image.'
}
if ($imageBytes.Length -gt 350KB) {
    throw 'Image exceeds 350 KiB; choose a smaller JPEG.'
}

Write-Host 'This sends the selected image to Alibaba Cloud Model Studio and uses model quota.'
$secureKey = Read-Host 'Enter the Model Studio API Key (input is hidden; do not send it in chat)' -AsSecureString
$keyPointer = [IntPtr]::Zero
try {
    $keyPointer = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secureKey)
    $apiKey = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($keyPointer)
    if ([string]::IsNullOrWhiteSpace($apiKey)) { throw 'API Key is empty.' }
    $imageUrl = 'data:image/jpeg;base64,' + [Convert]::ToBase64String($imageBytes)
    $body = @{
        model = 'qwen3.8-flash'
        enable_thinking = $false
        max_tokens = 512
        messages = @(@{
            role = 'user'
            content = @(
                @{ type = 'image_url'; image_url = @{ url = $imageUrl } },
                @{ type = 'text'; text = 'Identify the dominant cloud genus in the photo. Return JSON only, with fields type (cloud name in Simplified Chinese, or unknown), confidence_description (high, medium or low, indicating subjective judgment rather than a calibrated probability), and evidence (up to three short visual reasons in Simplified Chinese). If the sky or clouds are unclear, set type to unknown. Do not invent location or weather data.' }
            )
        })
    } | ConvertTo-Json -Depth 12 -Compress
    $headers = @{ Authorization = "Bearer $apiKey" }
    $httpResponse = Invoke-WebRequest -UseBasicParsing -Uri 'https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions' `
        -Method Post -Headers $headers -ContentType 'application/json; charset=utf-8' `
        -Body ([System.Text.Encoding]::UTF8.GetBytes($body)) -TimeoutSec 90
    # Windows PowerShell 5 may decode JSON with a legacy code page if the
    # server omits a charset. Decode the original response bytes as UTF-8.
    $responseBytes = $httpResponse.RawContentStream.ToArray()
    $response = [System.Text.Encoding]::UTF8.GetString($responseBytes) | ConvertFrom-Json
    Write-Host 'Model response:'
    Write-Output $response.choices[0].message.content
    if ($response.usage) {
        Write-Host ('Token usage: input {0}, output {1}' -f $response.usage.prompt_tokens, $response.usage.completion_tokens)
    }
} finally {
    if ($keyPointer -ne [IntPtr]::Zero) { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($keyPointer) }
    $apiKey = $null
    $headers = $null
    $secureKey = $null
}
