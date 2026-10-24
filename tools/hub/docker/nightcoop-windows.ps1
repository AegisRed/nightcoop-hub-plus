#Requires -Version 5.1
[CmdletBinding()]
param(
    [ValidateSet('menu','firewall','remove')] [string]$Action='menu',
    [string]$PeerIPs=''
)
$ErrorActionPreference='Stop'
$script:Base=Join-Path $PSScriptRoot 'docker-compose.yml'
$script:Windows=Join-Path $PSScriptRoot 'docker-compose.windows.yml'
$script:HubRule='NightCoop-Setup-Hub-UDP'
$script:LanRule='NightCoop-Setup-LAN-UDP'

function Say([string]$Text,[ConsoleColor]$Color='Gray') {
    Write-Host $Text -ForegroundColor $Color
}
function Is-Admin {
    $identity=[Security.Principal.WindowsIdentity]::GetCurrent()
    $principal=New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}
function Parse-Peers([string]$Value) {
    $result=@()
    foreach($part in ($Value -split '[,;\s]+' | Where-Object { $_ })) {
        $ip=$null
        if(-not [Net.IPAddress]::TryParse($part,[ref]$ip) -or
           $ip.AddressFamily -ne [Net.Sockets.AddressFamily]::InterNetwork) {
            throw "Invalid peer IPv4 address: $part"
        }
        if($result -notcontains $ip.ToString()) { $result+= $ip.ToString() }
    }
    return $result
}
function Install-Firewall([string]$Peers) {
    $validated=@(Parse-Peers $Peers)
    Say 'Two Windows Firewall rules on THIS computer:' Cyan
    Say '1. Hub: inbound UDP 47800,47810-47873; Private profile.' Yellow
    Say '2. LAN: inbound UDP 47800,47810-47824; LocalSubnet, any profile.' Yellow
    if($validated.Count) { Say ('Additional Radmin peer IPs: '+($validated -join ', ')) Yellow }
    if(-not (Is-Admin)) {
        Say 'Requesting administrator approval (UAC)...' Yellow
        $arguments='-NoProfile -ExecutionPolicy Bypass -File "'+$PSCommandPath+'" -Action firewall'
        if($validated.Count) { $arguments+=' -PeerIPs "'+($validated -join ',')+'"' }
        $child=Start-Process -FilePath 'powershell.exe' -ArgumentList $arguments -Verb RunAs -Wait -PassThru
        if($child.ExitCode -ne 0) { throw 'Firewall setup failed or UAC was declined.' }
        return
    }
    # Only our own named rules are replaced. Manually created rules are not touched.
    foreach($name in @($script:HubRule,$script:LanRule)) {
        $old=Get-NetFirewallRule -Name $name -ErrorAction SilentlyContinue
        if($old) { $old | Remove-NetFirewallRule }
    }
    $hub=@{
        Name=$script:HubRule; DisplayName='NightCoop Hub UDP (managed)'
        Description='NightCoop setup: entry and shard UDP ports.'
        Direction='Inbound'; Protocol='UDP'; LocalPort=@('47800','47810-47873')
        Action='Allow'; Profile='Private'
    }
    New-NetFirewallRule @hub | Out-Null
    $lan=@{
        Name=$script:LanRule; DisplayName='NightCoop LAN UDP (managed)'
        Description='NightCoop setup: LAN and optional explicit Radmin peers.'
        Direction='Inbound'; Protocol='UDP'; LocalPort=@('47800','47810-47824')
        RemoteAddress=(@('LocalSubnet')+$validated); Action='Allow'; Profile='Any'
    }
    New-NetFirewallRule @lan | Out-Null
    Say 'Both UDP rules are installed on this PC.' Green
    Say 'Repeat item 2 locally on your second PC; this tool never edits a remote PC.' Yellow
}
function Remove-Firewall {
    if(-not (Is-Admin)) {
        Say 'Requesting administrator approval (UAC) to remove helper rules...' Yellow
        $arguments='-NoProfile -ExecutionPolicy Bypass -File "'+$PSCommandPath+'" -Action remove'
        $child=Start-Process -FilePath 'powershell.exe' -ArgumentList $arguments -Verb RunAs -Wait -PassThru
        if($child.ExitCode -ne 0) { throw 'Firewall cleanup failed or UAC was declined.' }
        return
    }
    foreach($name in @($script:HubRule,$script:LanRule)) {
        $old=Get-NetFirewallRule -Name $name -ErrorAction SilentlyContinue
        if($old) { $old | Remove-NetFirewallRule }
    }
    Say 'Helper-created firewall rules removed. Manual rules were not changed.' Green
}
function Compose([string[]]$Subargs) {
    if(-not (Get-Command docker -ErrorAction SilentlyContinue)) {
        throw 'Docker CLI not found. Install/start Docker Desktop first.'
    }
    $all=@('compose','-f',$script:Base,'-f',$script:Windows)+$Subargs
    & docker @all
    if($LASTEXITCODE -ne 0) { throw "Docker Compose error, exit code $LASTEXITCODE." }
}
function Fingerprint {
    if(-not (Get-Command docker -ErrorAction SilentlyContinue)) { return $null }
    $a=@('compose','-f',$script:Base,'-f',$script:Windows,'logs','--no-color','--tail','300','hub')
    $lines=& docker @a 2>&1
    if($LASTEXITCODE -ne 0) { return $null }
    foreach($line in $lines) {
        if([string]$line -match 'fingerprint of this hub.s key is ([A-Z0-9-]+)') {
            return $Matches[1]
        }
    }
    return $null
}
function Help-Addresses {
    Say ''
    Say '===== HOW TO ENTER SERVER / INVITE IN CYBERPUNK =====' Cyan
    $fp=Fingerprint
    if(-not $fp) {
        $fp='<FINGERPRINT_FROM_LOGS>'
        Say 'Hub fingerprint not available yet. Start the hub or check the logs.' Yellow
    }
    Say 'HOST PC: Add an own server with:' Green
    Say ('  127.0.0.1#'+$fp) White

    $all=@(Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
        Where-Object { $_.IPAddress -notmatch '^(127\.|169\.254\.)' })
    $vpn=@($all | Where-Object { $_.InterfaceAlias -match '(?i)Radmin' })
    $lan=@($all | Where-Object {
        $_.InterfaceAlias -notmatch '(?i)Radmin|vEthernet|WSL|Docker|VirtualBox|VMware|Loopback' -and
        $_.IPAddress -match '^(10\.|192\.168\.|172\.(1[6-9]|2[0-9]|3[01])\.)'
    })
    Say 'LAN: give the friend the hosts LAN IPv4 address:' Green
    foreach($ip in $lan) { Say ('  '+$ip.IPAddress+' ['+$ip.InterfaceAlias+']') White }
    if(-not $lan.Count) { Say '  No LAN IPv4 found; inspect ipconfig on the host.' Yellow }
    $lanAddress=if($lan.Count) { $lan[0].IPAddress } else { '<HOST_LAN_IP>' }
    Say ('  Example: J3E5ZE@'+$lanAddress+'#'+$fp) Cyan
    Say 'RADMIN VPN: both PCs join the same Radmin network, use the HOSTS VPN IPv4:' Green
    foreach($ip in $vpn) { Say ('  '+$ip.IPAddress+' ['+$ip.InterfaceAlias+']') White }
    if(-not $vpn.Count) { Say '  No Radmin adapter found; open Radmin VPN and check the host VPN IPv4.' Yellow }
    $vpnAddress=if($vpn.Count) { $vpn[0].IPAddress } else { '<HOST_RADMIN_IP>' }
    Say ('  Example: J3E5ZE@'+$vpnAddress+'#'+$fp) Cyan
    Say ''
    Say 'J3E5ZE is an EXAMPLE. Use your real code copied from the game.' Yellow
    Say 'Replace ONLY the IP after @. Keep the original session code and #fingerprint.' Yellow
    Say 'This script does NOT generate or modify invitations.' DarkGray
    Say ''
}
function Start-Hub {
    Say 'Docker Desktop: bridge mode, UDP 47800 and 47810-47824.' Cyan
    $answer=Read-Host 'Install both firewall rules on this PC first? [Y/n]'
    if($answer -notmatch '^(n|no|н|нет)$') {
        $peers=Read-Host 'Optional Radmin peers IPv4 (comma-separated, Enter to skip)'
        Install-Firewall $peers
    }
    Compose @('config','--quiet')
    Compose @('up','-d','--build')
    Say 'Hub started:' Green
    Compose @('ps')
    # The first-start key and its fingerprint can appear a moment after 'up -d'.
    for($attempt=0;$attempt -lt 12;$attempt++) {
        if(Fingerprint) { break }
        Start-Sleep -Milliseconds 500
    }
    Help-Addresses
}
function Diagnostics {
    $stamp=Get-Date -Format 'yyyyMMdd-HHmmss'
    $path=Join-Path (Get-Location).Path "nightcoop-diagnostics-$stamp.txt"
    $lines=New-Object 'System.Collections.Generic.List[string]'
    $lines.Add('NightCoop diagnostic report '+(Get-Date -Format 'u'))
    $lines.Add('OS: '+[Environment]::OSVersion.VersionString)
    $lines.Add('=== IPv4 ===')
    $lines.Add([string](Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
        Select-Object InterfaceAlias,IPAddress,PrefixLength | Format-Table -AutoSize | Out-String))
    $lines.Add('=== Setup-created Windows Firewall rules ===')
    foreach($name in @($script:HubRule,$script:LanRule)) {
        $rule=Get-NetFirewallRule -Name $name -ErrorAction SilentlyContinue
        if(-not $rule) { $lines.Add("$name is missing"); continue }
        $lines.Add([string]($rule | Select-Object Name,Enabled,Profile,Direction,Action | Format-List | Out-String))
        $lines.Add([string]($rule | Get-NetFirewallPortFilter | Format-List | Out-String))
        $lines.Add([string]($rule | Get-NetFirewallAddressFilter | Format-List | Out-String))
    }
    if(Get-Command docker -ErrorAction SilentlyContinue) {
        $args2=@('compose','-f',$script:Base,'-f',$script:Windows)
        $lines.Add('=== Docker Compose status ===')
        $lines.Add([string]((& docker @args2 ps 2>&1) | Out-String))
        $lines.Add('=== Last 200 hub log lines ===')
        $lines.Add([string]((& docker @args2 logs --no-color --tail 200 hub 2>&1) | Out-String))
    } else {
        $lines.Add('Docker CLI not found.')
    }
    $lines | Set-Content -Path $path -Encoding UTF8
    Say ("Saved: $path") Green
    Say 'Contains local/VPN IPs and possibly session data. Review before sharing.' Yellow
}
function Menu {
    while($true) {
        Say ''
        Say '========== NIGHTCOOP WINDOWS ==========' Cyan
        Say '1. Start/update hub (Docker Desktop + Firewall)' White
        Say '2. Install BOTH Firewall rules on THIS PC (also for guest PC)' White
        Say '3. LAN/Radmin address instructions for the game' White
        Say '4. Live logs (Ctrl+C to exit)' White
        Say '5. Export troubleshooting log' White
        Say '6. Stop hub (preserve signing key)' White
        Say '7. Remove the two helper-created Firewall rules' White
        Say '0. Exit' Gray
        $choice=Read-Host 'Choose'
        try {
            switch($choice) {
                '1' { Start-Hub }
                '2' {
                    $peers=Read-Host 'Optional Radmin peers IPv4 (comma-separated, Enter to skip)'
                    Install-Firewall $peers
                }
                '3' { Help-Addresses }
                '4' { Compose @('logs','--tail','100','-f','hub') }
                '5' { Diagnostics }
                '6' { Compose @('stop','hub'); Say 'Stopped; key volume preserved.' Green }
                '7' { Remove-Firewall }
                '0' { return }
                default { Say 'Unknown menu item.' Yellow }
            }
        } catch {
            Say ('ERROR: '+$_.Exception.Message) Red
        }
    }
}
try {
    if($Action -eq 'firewall') { Install-Firewall $PeerIPs }
    elseif($Action -eq 'remove') { Remove-Firewall }
    else { Menu }
} catch {
    Say ('ERROR: '+$_.Exception.Message) Red
    exit 1
}
