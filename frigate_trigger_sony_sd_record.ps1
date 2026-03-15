param(
    [string]$FrigateApiBase = "https://10.0.0.101:30193",
    [string]$FrigateCamera = "sony_on_blk_lptp",
    [string]$FrigateUsername = "sonyguard",
    [string]$FrigatePassword = "SonyGuard!2026",
    [string]$FrigateAuthToken = "",
    [string]$BridgeBaseUrl = "http://127.0.0.1:8770",
    [string]$LabelFilter = "person",
    [int]$NoMotionStopSec = 30,
    [int]$PollSec = 1,
    [int]$StartRetrySec = 12
)

$ErrorActionPreference = "Stop"

[System.Net.ServicePointManager]::ServerCertificateValidationCallback = { $true }

$script:FrigateSessionToken = $null
$script:FrigateSessionTokenTs = [datetime]::MinValue

function Get-FrigateAuthHeaders {
    if (-not [string]::IsNullOrWhiteSpace($FrigateAuthToken)) {
        return @{ "Authorization" = "Bearer $FrigateAuthToken" }
    }

    if ([string]::IsNullOrWhiteSpace($FrigateUsername)) {
        return @{}
    }

    $isFresh = $script:FrigateSessionToken -and (((Get-Date) - $script:FrigateSessionTokenTs).TotalHours -lt 24)
    if (-not $isFresh) {
        $loginUrl = "$FrigateApiBase/api/login"
        $body = @{ user = $FrigateUsername; password = $FrigatePassword } | ConvertTo-Json -Compress
        $resp = Invoke-WebRequest -Method Post -Uri $loginUrl -ContentType "application/json" -Body $body -TimeoutSec 8
        $setCookie = [string]$resp.Headers["Set-Cookie"]
        if ([string]::IsNullOrWhiteSpace($setCookie)) {
            throw "Frigate login succeeded but no Set-Cookie token was returned."
        }
        $m = [regex]::Match($setCookie, 'frigate_token=([^;]+)')
        if (-not $m.Success) {
            throw "Frigate login did not return frigate_token cookie."
        }
        $script:FrigateSessionToken = $m.Groups[1].Value
        $script:FrigateSessionTokenTs = Get-Date
    }

    return @{ "Cookie" = "frigate_token=$($script:FrigateSessionToken)" }
}

function Get-ActiveDetections {
    param(
        [string]$ApiBase,
        [string]$Camera,
        [string]$Label
    )

    $url = "$ApiBase/api/events?camera=$([uri]::EscapeDataString($Camera))&in_progress=1&limit=50"
    $headers = Get-FrigateAuthHeaders

    $events = Invoke-RestMethod -Method Get -Uri $url -Headers $headers -TimeoutSec 5
    if ($null -eq $events) {
        return @()
    }

    $arr = @($events)
    if ([string]::IsNullOrWhiteSpace($Label)) {
        return $arr
    }

    return @($arr | Where-Object { $_.label -eq $Label })
}

function Invoke-BridgeMovieCall {
    param(
        [string]$BridgeUrl,
        [string]$Endpoint
    )

    $target = "$BridgeUrl$Endpoint"
    Invoke-RestMethod -Method Post -Uri $target -TimeoutSec 8 | Out-Null
}

$recording = $false
$lastMotion = [datetime]::MinValue
$lastStartAttempt = [datetime]::MinValue

Write-Output "Frigate SD trigger loop started. Frigate=$FrigateApiBase Camera=$FrigateCamera Label=$LabelFilter NoMotionStopSec=$NoMotionStopSec PollSec=$PollSec StartRetrySec=$StartRetrySec Auth=$(if ($FrigateAuthToken) { 'bearer' } elseif ($FrigateUsername) { 'session-cookie' } else { 'none' })"

while ($true) {
    try {
        $active = Get-ActiveDetections -ApiBase $FrigateApiBase -Camera $FrigateCamera -Label $LabelFilter
        $activeCount = @($active).Count

        if ($activeCount -gt 0) {
            $lastMotion = Get-Date
            if (-not $recording) {
                $sinceLastStart = ((Get-Date) - $lastStartAttempt).TotalSeconds
                if ($sinceLastStart -ge $StartRetrySec) {
                    $lastStartAttempt = Get-Date
                    Invoke-BridgeMovieCall -BridgeUrl $BridgeBaseUrl -Endpoint "/api/start_movie"
                    $recording = $true
                    Write-Output "[$((Get-Date).ToString('s'))] Motion detected ($activeCount). Camera SD recording started."
                }
            }
        } elseif ($recording) {
            $idleFor = ((Get-Date) - $lastMotion).TotalSeconds
            if ($idleFor -ge $NoMotionStopSec) {
                Invoke-BridgeMovieCall -BridgeUrl $BridgeBaseUrl -Endpoint "/api/stop_movie"
                $recording = $false
                Write-Output "[$((Get-Date).ToString('s'))] No motion for ${NoMotionStopSec}s. Camera SD recording stopped."
            }
        }
    } catch {
        Write-Output "[$((Get-Date).ToString('s'))] WARN: $($_.Exception.Message)"
    }

    Start-Sleep -Seconds $PollSec
}
