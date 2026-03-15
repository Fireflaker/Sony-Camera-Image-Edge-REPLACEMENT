param(
    [string]$CameraIp = "192.168.122.1",
    [string]$WifiInterface = "WLAN 2",
    [string]$WifiPassword = "QDvADbLp",
    [int]$HttpPort = 8770,
    [int]$RtspPort = 8554,
    [string]$RtspPath = "sony",
    [int]$OutputWidth = 0,
    [int]$OutputHeight = 0,
    [int]$OutputFps = 0,
    [int]$OutputCrf = 21,
    [bool]$UseNvenc = $false,
    [bool]$EnableAutoSdRecord = $true,
    [string]$FrigateApiBase = "https://10.0.0.101:30193",
    [string]$FrigateCamera = "sony_on_blk_lptp",
    [string]$FrigateUsername = "sonyguard",
    [string]$FrigatePassword = "SonyGuard!2026",
    [string]$FrigateAuthToken = "",
    [string]$FrigateLabel = "person",
    [int]$NoMotionStopSec = 30,
    [int]$DetectionPollSec = 1
)

$ErrorActionPreference = "Stop"

$root = "E:\Co2Root\ImagingEdge4Linux"
$pythonExe = "e:\Co2Root\.venv\Scripts\python.exe"
$liveviewPy = "$root\liveview_webui.py"
$mediamtxExe = "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\bluenviron.mediamtx_Microsoft.Winget.Source_8wekyb3d8bbwe\mediamtx.exe"
$ffmpegExe = "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\Gyan.FFmpeg_Microsoft.Winget.Source_8wekyb3d8bbwe\ffmpeg-8.0.1-full_build\bin\ffmpeg.exe"
$mediamtxCfg = "$root\mediamtx.yml"
$relayLoopScript = "E:\Co2Root\ffmpeg_rtsp_relay_loop.ps1"
$relayPidFile = "E:\Co2Root\ffmpeg_rtsp_relay_loop.pid"
$autoRecScript = "E:\Co2Root\frigate_trigger_sony_sd_record.ps1"
$autoRecPidFile = "E:\Co2Root\frigate_trigger_sony_sd_record.pid"
$ffprobeExe = "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\Gyan.FFmpeg_Microsoft.Winget.Source_8wekyb3d8bbwe\ffmpeg-8.0.1-full_build\bin\ffprobe.exe"

function Wait-HttpReady {
    param(
        [string]$Url,
        [int]$TimeoutSec = 20
    )
    $end = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $end) {
        try {
            $r = Invoke-WebRequest -UseBasicParsing -Uri $Url -TimeoutSec 3
            if ($r.StatusCode -ge 200 -and $r.StatusCode -lt 500) {
                return $true
            }
        } catch {
        }
        Start-Sleep -Milliseconds 500
    }
    return $false
}

function Wait-FrameReady {
    param(
        [string]$BaseUrl,
        [int]$TimeoutSec = 30
    )
    $end = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $end) {
        try {
            $frame = Invoke-WebRequest -UseBasicParsing -Uri "$BaseUrl/frame.jpg?t=$([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds())" -TimeoutSec 4
            if ($frame.StatusCode -eq 200 -and $frame.Content.Length -gt 5000) {
                return $true
            }
        } catch {
        }
        Start-Sleep -Milliseconds 700
    }
    return $false
}

@"
paths:
  ${RtspPath}:
    source: publisher
"@ | Set-Content -Path $mediamtxCfg -Encoding ascii

# Firewall rules for TrueNAS -> laptop access
try {
    if (-not (Get-NetFirewallRule -DisplayName "SonyBridgeHTTP$HttpPort" -ErrorAction SilentlyContinue)) {
        New-NetFirewallRule -DisplayName "SonyBridgeHTTP$HttpPort" -Direction Inbound -Action Allow -Protocol TCP -LocalPort $HttpPort | Out-Null
    }
} catch {
    Write-Output "WARN: Could not create firewall rule SonyBridgeHTTP$HttpPort (run shell as Administrator if needed)."
}
try {
    if (-not (Get-NetFirewallRule -DisplayName "SonyBridgeRTSP$RtspPort" -ErrorAction SilentlyContinue)) {
        New-NetFirewallRule -DisplayName "SonyBridgeRTSP$RtspPort" -Direction Inbound -Action Allow -Protocol TCP -LocalPort $RtspPort | Out-Null
    }
} catch {
    Write-Output "WARN: Could not create firewall rule SonyBridgeRTSP$RtspPort (run shell as Administrator if needed)."
}

# Restart liveview bridge cleanly so it always uses latest settings
$httpPids = Get-NetTCPConnection -LocalPort $HttpPort -State Listen -ErrorAction SilentlyContinue | Select-Object -ExpandProperty OwningProcess -Unique
foreach ($p in $httpPids) {
    Stop-Process -Id $p -Force -ErrorAction SilentlyContinue
}
$liveviewArgs = "$liveviewPy --address $CameraIp --wifi-interface `"$WifiInterface`" --wifi-password $WifiPassword --listen 0.0.0.0 --port $HttpPort"
Start-Process -FilePath $pythonExe -ArgumentList $liveviewArgs -WorkingDirectory $root | Out-Null

$baseUrl = "http://127.0.0.1:$HttpPort"
if (-not (Wait-HttpReady -Url "$baseUrl/api/status" -TimeoutSec 30)) {
    throw "Liveview bridge did not start on port $HttpPort"
}

# Ensure camera is actually connected/streaming before relay starts
try {
    Invoke-RestMethod -Method Post -Uri "$baseUrl/api/start_liveview" -TimeoutSec 10 | Out-Null
} catch {
}

if (-not (Wait-FrameReady -BaseUrl $baseUrl -TimeoutSec 45)) {
    $statusText = ""
    try {
        $statusText = (Invoke-WebRequest -UseBasicParsing -Uri "$baseUrl/api/status" -TimeoutSec 5).Content
    } catch {
    }
    throw "Camera frame not ready. Put camera in Ctrl w/ Smartphone mode and check WLAN adapter '$WifiInterface'. Status: $statusText"
}

# Restart MediaMTX cleanly
foreach ($p in (Get-NetTCPConnection -LocalPort $RtspPort -State Listen -ErrorAction SilentlyContinue | Select-Object -ExpandProperty OwningProcess -Unique)) {
    Stop-Process -Id $p -Force -ErrorAction SilentlyContinue
}
Start-Process -FilePath $mediamtxExe -ArgumentList $mediamtxCfg -WorkingDirectory $root | Out-Null
Start-Sleep -Seconds 2

# Restart ffmpeg relay cleanly (convert MJPEG HTTP -> H264 RTSP)
Get-Process -Name ffmpeg -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
# stop existing relay loop process if present
if (Test-Path $relayPidFile) {
    $oldPid = Get-Content $relayPidFile -ErrorAction SilentlyContinue
    if ($oldPid) {
        Stop-Process -Id $oldPid -Force -ErrorAction SilentlyContinue
    }
    Remove-Item $relayPidFile -Force -ErrorAction SilentlyContinue
}

# stop existing SD-trigger loop process if present
if (Test-Path $autoRecPidFile) {
    $oldPid = Get-Content $autoRecPidFile -ErrorAction SilentlyContinue
    if ($oldPid) {
        Stop-Process -Id $oldPid -Force -ErrorAction SilentlyContinue
    }
    Remove-Item $autoRecPidFile -Force -ErrorAction SilentlyContinue
}

$inputUrl = "http://127.0.0.1:$HttpPort/stream"
$outputUrl = "rtsp://127.0.0.1:$RtspPort/$RtspPath"
$nvencArg = if ($UseNvenc) { " -UseNvenc" } else { "" }
$loopArgs = "-ExecutionPolicy Bypass -File `"$relayLoopScript`" -FfmpegExe `"$ffmpegExe`" -InputUrl `"$inputUrl`" -OutputRtspUrl `"$outputUrl`" -Width $OutputWidth -Height $OutputHeight -Fps $OutputFps -Crf $OutputCrf$nvencArg"
$loopProc = Start-Process -FilePath "powershell" -ArgumentList $loopArgs -WindowStyle Hidden -PassThru
$loopProc.Id | Set-Content -Path $relayPidFile -Encoding ascii
Start-Sleep -Seconds 2

if ($EnableAutoSdRecord) {
    if (-not (Test-Path $autoRecScript)) {
        Write-Output "WARN: Auto SD trigger script not found: $autoRecScript"
    } else {
        $autoArgs = "-ExecutionPolicy Bypass -File `"$autoRecScript`" -FrigateApiBase `"$FrigateApiBase`" -FrigateCamera `"$FrigateCamera`" -BridgeBaseUrl `"$baseUrl`" -LabelFilter `"$FrigateLabel`" -NoMotionStopSec $NoMotionStopSec -PollSec $DetectionPollSec"
        if (-not [string]::IsNullOrWhiteSpace($FrigateUsername)) {
            $autoArgs += " -FrigateUsername `"$FrigateUsername`""
            $autoArgs += " -FrigatePassword `"$FrigatePassword`""
        }
        if (-not [string]::IsNullOrWhiteSpace($FrigateAuthToken)) {
            $autoArgs += " -FrigateAuthToken `"$FrigateAuthToken`""
        }
        $autoProc = Start-Process -FilePath "powershell" -ArgumentList $autoArgs -WindowStyle Hidden -PassThru
        $autoProc.Id | Set-Content -Path $autoRecPidFile -Encoding ascii
    }
}

# Validate RTSP publishes a readable stream
if (Test-Path $ffprobeExe) {
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $probe = & $ffprobeExe -v error -rtsp_transport tcp -show_entries stream=codec_name,width,height -of default=noprint_wrappers=1 "rtsp://127.0.0.1:$RtspPort/$RtspPath" 2>&1
        if ($LASTEXITCODE -ne 0) {
            Write-Output "WARN: RTSP probe failed. Relay may still be warming up."
            Write-Output $probe
        } else {
            Write-Output $probe
        }
    } finally {
        $ErrorActionPreference = $prevEap
    }
}

$lan = (Get-NetIPAddress -AddressFamily IPv4 | Where-Object { $_.IPAddress -like "10.*" -and $_.PrefixLength -eq 24 } | Select-Object -First 1 -ExpandProperty IPAddress)
if (-not $lan) {
    $lan = (Get-NetIPAddress -AddressFamily IPv4 | Where-Object { $_.IPAddress -like "192.168.*" } | Select-Object -First 1 -ExpandProperty IPAddress)
}

Write-Output "HTTP bridge: http://${lan}:$HttpPort/stream"
Write-Output "RTSP relay : rtsp://${lan}:$RtspPort/$RtspPath"
Write-Output "Frigate wizard URL: rtsp://${lan}:$RtspPort/$RtspPath"
if ($OutputWidth -gt 0 -and $OutputHeight -gt 0) {
    if ($OutputFps -gt 0) {
        Write-Output "Relay profile: ${OutputWidth}x${OutputHeight} @ ${OutputFps}fps (CRF ${OutputCrf})"
    } else {
        Write-Output "Relay profile: ${OutputWidth}x${OutputHeight} @ source fps (CRF ${OutputCrf})"
    }
} else {
    if ($OutputFps -gt 0) {
        Write-Output "Relay profile: source resolution @ ${OutputFps}fps (CRF ${OutputCrf})"
    } else {
        Write-Output "Relay profile: source resolution @ source fps (best quality/efficiency baseline)"
    }
}
Write-Output "Encoder preference: $(if ($UseNvenc) { 'h264_nvenc (auto-fallback to libx264)' } else { 'libx264' })"
Write-Output "Recommendation: use RTSP for Frigate (lower bandwidth, better efficiency); keep HTTP as fallback/debug."
if ($EnableAutoSdRecord) {
    $authMode = if ($FrigateAuthToken) { 'bearer' } elseif ($FrigateUsername) { 'session-cookie' } else { 'none' }
    Write-Output "SD trigger loop: enabled (Frigate=$FrigateApiBase, Camera=$FrigateCamera, Label=$FrigateLabel, StopAfterNoMotion=${NoMotionStopSec}s, Auth=$authMode)"
} else {
    Write-Output "SD trigger loop: disabled"
}
