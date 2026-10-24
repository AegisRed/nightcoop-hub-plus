#Requires -Version 5.1
# NightCoop read-only dashboard. TCP listener is bound ONLY to 127.0.0.1.
# No Docker socket mount, no command endpoints, no remote access and no secret-key file access.
[CmdletBinding()]
param([ValidateRange(1024,65535)][int]$Port=47990)
$ErrorActionPreference='Stop'
$composeFile=Join-Path $PSScriptRoot 'docker-compose.windows.yml'
$htmlFile=Join-Path $PSScriptRoot 'dashboard.html'
$dockerContainer='nightcoop-hub'
$env:Path=[Environment]::GetEnvironmentVariable('Path','Machine')+';'+[Environment]::GetEnvironmentVariable('Path','User')+';'+$env:Path
foreach($candidate in @(
    (Join-Path $env:ProgramFiles 'Docker\Docker\resources\bin'),
    (Join-Path $env:LOCALAPPDATA 'Programs\DockerDesktop\resources\bin')
)) {
    if(Test-Path (Join-Path $candidate 'docker.exe')) { $env:Path+=';'+$candidate }
}
function Docker-Output([string[]]$Arguments) {
    if(-not (Get-Command docker -ErrorAction SilentlyContinue)) {
        return @{ success=$false; lines=@('Docker CLI not installed') }
    }
    try {
        $lines=@(& docker @Arguments 2>&1 | ForEach-Object { [string]$_ })
        return @{ success=($LASTEXITCODE -eq 0); lines=$lines }
    } catch {
        return @{ success=$false; lines=@($_.Exception.Message) }
    }
}
function Get-HubLogs {
    $output=Docker-Output @('logs','--tail','250','--timestamps',$dockerContainer)
    if(-not $output.success) {
        return @{ success=$false; text=($output.lines -join [Environment]::NewLine) }
    }
    $joined=($output.lines -join [Environment]::NewLine)
    if($joined.Length -gt 120000) { $joined=$joined.Substring($joined.Length-120000) }
    return @{ success=$true; text=$joined }
}
function Get-Info {
    $inspect=Docker-Output @('inspect','--format','{{.State.Running}}',$dockerContainer)
    $running=$inspect.success -and (($inspect.lines -join '').Trim() -eq 'true')
    $portOut=Docker-Output @('port',$dockerContainer,'47800/udp')
    $udpPublished=$portOut.success -and ($portOut.lines.Count -gt 0)
    $log=Get-HubLogs
    $fingerprint=''
    foreach($line in ($log.text -split '\r?\n')) {
        if($line -match 'fingerprint of this hub.s key is ([A-Z0-9-]+)') {
            $fingerprint=$Matches[1]
        }
    }
    $all=@(Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
        Where-Object { $_.IPAddress -notmatch '^(127\.|169\.254\.)' })
    $lan=@($all | Where-Object {
        $_.InterfaceAlias -notmatch '(?i)Radmin|vEthernet|WSL|Docker|VirtualBox|VMware|Loopback' -and
        $_.IPAddress -match '^(10\.|192\.168\.|172\.(1[6-9]|2[0-9]|3[01])\.)'
    } | ForEach-Object { @{ ip=$_.IPAddress; adapter=$_.InterfaceAlias } })
    $radmin=@($all | Where-Object { $_.InterfaceAlias -match '(?i)Radmin' } |
        ForEach-Object { @{ ip=$_.IPAddress; adapter=$_.InterfaceAlias } })
    $firewallHub=[bool](Get-NetFirewallRule -Name 'NightCoop-Setup-Hub-UDP' -ErrorAction SilentlyContinue)
    $firewallLan=[bool](Get-NetFirewallRule -Name 'NightCoop-Setup-LAN-UDP' -ErrorAction SilentlyContinue)
    return @{
        running=[bool]$running
        udpPublished=[bool]$udpPublished
        fingerprint=$fingerprint
        lan=$lan
        radmin=$radmin
        firewall=@{ hub=$firewallHub; lan=$firewallLan }
        checkedAt=(Get-Date).ToString('o')
        error= if(-not $running) { 'Hub container is not running. Start it with INSTALL-WINDOWS.cmd or the menu.' }
               elseif(-not $udpPublished) { 'UDP 47800 mapping was not found.' }
               else { '' }
    }
}
function Send-Response(
    [System.Net.Sockets.NetworkStream]$Stream,
    [int]$Code,
    [string]$Reason,
    [string]$ContentType,
    [string]$Body
) {
    $bodyBytes=[Text.Encoding]::UTF8.GetBytes($Body)
    $headers="HTTP/1.1 $Code $Reason" + [char]13+[char]10
    $headers+="Content-Type: $ContentType" + [char]13+[char]10
    $headers+="Content-Length: $($bodyBytes.Length)" + [char]13+[char]10
    $headers+="Cache-Control: no-store" + [char]13+[char]10
    $headers+="X-Content-Type-Options: nosniff" + [char]13+[char]10
    $headers+="X-Frame-Options: DENY" + [char]13+[char]10
    $headers+="Referrer-Policy: no-referrer" + [char]13+[char]10
    $headers+="Content-Security-Policy: default-src 'none'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; connect-src 'self'; img-src 'self'; base-uri 'none'; frame-ancestors 'none'" + [char]13+[char]10
    $headers+="Connection: close" + [char]13+[char]10+[char]13+[char]10
    $headerBytes=[Text.Encoding]::ASCII.GetBytes($headers)
    $Stream.Write($headerBytes,0,$headerBytes.Length)
    $Stream.Write($bodyBytes,0,$bodyBytes.Length)
    $Stream.Flush()
}
function Handle-Request([System.Net.Sockets.TcpClient]$Client) {
    $Client.ReceiveTimeout=5000
    $Client.SendTimeout=10000
    $stream=$Client.GetStream()
    $reader=New-Object System.IO.StreamReader($stream,[Text.Encoding]::ASCII,$false,1024,$true)
    $first=$reader.ReadLine()
    if(-not $first -or $first.Length -gt 2048) { return }
    $headers=@{}
    for($i=0;$i -lt 64;$i++) {
        $line=$reader.ReadLine()
        if($null -eq $line -or $line -eq '') { break }
        if($line.Length -gt 4096) { return }
        $index=$line.IndexOf(':')
        if($index -gt 0) { $headers[$line.Substring(0,$index).ToLowerInvariant()]=$line.Substring($index+1).Trim() }
    }
    # Host/origin validation protects local data from remote-origin web pages and DNS rebinding.
    $allowedHosts=@("127.0.0.1:$Port","localhost:$Port")
    $allowedOrigins=@("http://127.0.0.1:$Port","http://localhost:$Port")
    if($headers['host'] -notin $allowedHosts -or
       ($headers.ContainsKey('origin') -and $headers['origin'] -notin $allowedOrigins)) {
        Send-Response $stream 403 'Forbidden' 'text/plain; charset=utf-8' 'Local dashboard only'
        return
    }
    if($first -notmatch '^GET (/[A-Za-z0-9./_-]*) HTTP/1\.[01]$') {
        Send-Response $stream 405 'Method Not Allowed' 'text/plain; charset=utf-8' 'Read-only GET endpoints'
        return
    }
    $path=$Matches[1]
    switch($path) {
        '/' {
            $html=Get-Content -LiteralPath $htmlFile -Raw -Encoding UTF8
            Send-Response $stream 200 'OK' 'text/html; charset=utf-8' $html
        }
        '/health' {
            Send-Response $stream 200 'OK' 'text/plain; charset=utf-8' 'NightCoop Dashboard'
        }
        '/api/status' {
            $json=Get-Info | ConvertTo-Json -Depth 6 -Compress
            Send-Response $stream 200 'OK' 'application/json; charset=utf-8' $json
        }
        '/api/logs' {
            $json=Get-HubLogs | ConvertTo-Json -Depth 4 -Compress
            Send-Response $stream 200 'OK' 'application/json; charset=utf-8' $json
        }
        default {
            Send-Response $stream 404 'Not Found' 'text/plain; charset=utf-8' 'Not found'
        }
    }
}
$listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,$Port)
$listener.Start()
try {
    Write-Host "NightCoop local dashboard listening at http://127.0.0.1:$Port"
    while($true) {
        $client=$listener.AcceptTcpClient()
        try { Handle-Request $client }
        catch { Write-Warning ("Dashboard request failed: "+$_.Exception.Message) }
        finally { $client.Close() }
    }
} finally {
    $listener.Stop()
}
