param(
    [string]$CameraModelHint = 'ILCE-6400',
    [string]$WifiPassword,
    [string]$PreferredInterface = 'WLAN 2',
    [int]$MaxAttempts = 180,
    [int]$ScanIntervalSeconds = 5,
    [string]$OutFile = 'e:\Co2Root\liveview-url.txt',
    [string]$LogFile = 'e:\Co2Root\liveview-acquire.log'
)

$ErrorActionPreference = 'SilentlyContinue'

function Log([string]$msg) {
    $line = "[$(Get-Date -Format s)] $msg"
    $line | Out-File -FilePath $LogFile -Append -Encoding utf8
    Write-Output $line
}

function Call-SonyApi($ip, $service, $method, $params = @(@()), $version = '1.0') {
    $body = @{ method = $method; params = $params; id = 1; version = $version } | ConvertTo-Json -Compress -Depth 8
    try {
        return Invoke-RestMethod -Uri "http://$ip:10000/sony/$service" -Method Post -ContentType 'application/json' -Body $body -TimeoutSec 4
    } catch {
        return $null
    }
}

function Find-CameraSsid([string[]]$interfaces, [string]$modelHint) {
    foreach ($ifn in $interfaces) {
        $scan = netsh wlan show networks mode=bssid interface="$ifn" | Out-String
        $m = [regex]::Match($scan, "DIRECT-[^\r\n]*$([regex]::Escape($modelHint))")
        if ($m.Success) {
            return [pscustomobject]@{ Interface = $ifn; Ssid = $m.Value }
        }
    }
    return $null
}

function Connect-CameraWifi($iface, $ssid, $password) {
    if (-not $password) {
        throw 'Missing Wi-Fi password. Pass -WifiPassword.'
    }

    $xmlPath = 'e:\Co2Root\sony-liveview-temp.xml'
    $xml = @"
<?xml version="1.0"?>
<WLANProfile xmlns="http://www.microsoft.com/networking/WLAN/profile/v1">
  <name>$ssid</name>
  <SSIDConfig><SSID><name>$ssid</name></SSID></SSIDConfig>
  <connectionType>ESS</connectionType>
  <connectionMode>manual</connectionMode>
  <MSM>
    <security>
      <authEncryption>
        <authentication>WPA2PSK</authentication>
        <encryption>AES</encryption>
        <useOneX>false</useOneX>
      </authEncryption>
      <sharedKey>
        <keyType>passPhrase</keyType>
        <protected>false</protected>
        <keyMaterial>$password</keyMaterial>
      </sharedKey>
    </security>
  </MSM>
</WLANProfile>
"@

    Set-Content -Path $xmlPath -Value $xml -Encoding ASCII
    netsh wlan add profile filename="$xmlPath" interface="$iface" user=current | Out-Null
    netsh wlan connect name="$ssid" ssid="$ssid" interface="$iface" | Out-Null
    Start-Sleep -Seconds 7
}

Remove-Item $OutFile,$LogFile -ErrorAction SilentlyContinue

$interfaces = @($PreferredInterface, 'WLAN', 'WLAN 2') | Select-Object -Unique

for ($i = 0; $i -lt $MaxAttempts; $i++) {
    Log "scan attempt $i"

    $hit = Find-CameraSsid -interfaces $interfaces -modelHint $CameraModelHint
    if (-not $hit) {
        Start-Sleep -Seconds $ScanIntervalSeconds
        continue
    }

    Log "found SSID '$($hit.Ssid)' on interface '$($hit.Interface)'"
    Connect-CameraWifi -iface $hit.Interface -ssid $hit.Ssid -password $WifiPassword

    $gw = (Get-NetIPConfiguration -InterfaceAlias $hit.Interface).IPv4DefaultGateway.NextHop
    $candidates = @($gw, '192.168.122.1', '10.0.0.1') | Where-Object { $_ } | Select-Object -Unique

    foreach ($ip in $candidates) {
        $v = Call-SonyApi -ip $ip -service 'camera' -method 'getVersions'
        if (-not $v) { continue }

        Log "Sony API detected at $ip"
        $null = Call-SonyApi -ip $ip -service 'camera' -method 'startRecMode'
        Start-Sleep -Seconds 2

        $apis = Call-SonyApi -ip $ip -service 'camera' -method 'getAvailableApiList'
        if ($apis) {
            Log ("APIs: " + ($apis | ConvertTo-Json -Compress -Depth 6))
        }

        $lv = Call-SonyApi -ip $ip -service 'camera' -method 'startLiveview'
        if ($lv -and $lv.result -and $lv.result[0]) {
            $url = $lv.result[0]
            Set-Content -Path $OutFile -Value $url -Encoding ascii
            Log "LIVEVIEW_URL=$url"
            Write-Output "SUCCESS: $url"
            exit 0
        }

        Log "startLiveview not available yet at $ip"
    }

    Start-Sleep -Seconds 3
}

Log 'Timeout waiting for camera liveview API.'
exit 1
