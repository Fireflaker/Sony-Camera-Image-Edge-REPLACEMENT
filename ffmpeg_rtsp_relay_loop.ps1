param(
    [string]$FfmpegExe,
    [string]$InputUrl,
    [string]$OutputRtspUrl,
    [int]$Width = 0,
    [int]$Height = 0,
    [int]$Fps = 0,
    [int]$Crf = 21,
    [bool]$UseNvenc = $false,
    [string]$LogFile = "E:\Co2Root\ImagingEdge4Linux\ffmpeg_relay_loop.log"
)

function Get-EncoderArgs {
    param(
        [string]$Exe,
        [int]$QualityCrf,
        [bool]$PreferNvenc
    )

    if ($PreferNvenc) {
        try {
            $encoders = & $Exe -hide_banner -encoders 2>&1 | Out-String
            if ($encoders -match "h264_nvenc") {
                return @('-c:v', 'h264_nvenc', '-preset', 'p5', '-tune', 'll', '-rc', 'vbr', '-cq', '19', '-b:v', '0')
            }
        } catch {
        }
    }

    return @(
        '-c:v', 'libx264',
        '-preset', 'ultrafast',
        '-tune', 'zerolatency',
        '-crf', "$QualityCrf",
        '-x264-params', 'bframes=0:rc-lookahead=0:scenecut=0:keyint=30:min-keyint=30',
        '-bf', '0'
    )
}

while ($true) {
    "[$(Get-Date -Format s)] starting ffmpeg relay" | Out-File -FilePath $LogFile -Append -Encoding utf8
    $ffArgs = @(
        '-hide_banner', '-loglevel', 'warning',
        '-fflags', 'nobuffer', '-flags', 'low_delay',
        '-rw_timeout', '5000000', '-reconnect', '1', '-reconnect_streamed', '1', '-reconnect_delay_max', '2',
        '-f', 'mpjpeg', '-i', $InputUrl
    )

    # Keep source quality by default; only transform when explicitly requested.
    $filters = @()
    if ($Fps -gt 0) {
        $filters += "fps=$Fps"
    }
    if ($Width -gt 0 -and $Height -gt 0) {
        $filters += "scale=${Width}:${Height}:flags=lanczos"
    }
    if ($filters.Count -gt 0) {
        $ffArgs += @('-vf', ($filters -join ','))
    }

    $ffArgs += @('-an')
    $ffArgs += Get-EncoderArgs -Exe $FfmpegExe -QualityCrf $Crf -PreferNvenc ([bool]$UseNvenc)

    if ($Fps -gt 0) {
        $ffArgs += @('-g', "$Fps", '-keyint_min', "$Fps")
    }

    $ffArgs += @(
        '-pix_fmt', 'yuv420p',
        '-muxdelay', '0',
        '-muxpreload', '0',
        '-f', 'rtsp',
        '-rtsp_transport', 'tcp',
        '-pkt_size', '1200',
        $OutputRtspUrl
    )

    & $FfmpegExe @ffArgs 2>&1 | Out-File -FilePath $LogFile -Append -Encoding utf8
    "[$(Get-Date -Format s)] ffmpeg exited, restarting in 1s" | Out-File -FilePath $LogFile -Append -Encoding utf8
    Start-Sleep -Seconds 1
}
