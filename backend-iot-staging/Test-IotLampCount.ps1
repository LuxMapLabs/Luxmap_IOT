$ErrorActionPreference='Stop'
$repo='E:\Ki9\DoAn\Github\luxmap_backend'
$out=Join-Path $env:TEMP 'luxmap-iot-25-build'
$fixture=Join-Path $env:TEMP ('luxmap-iot-fixture-'+[guid]::NewGuid().ToString('N'))
$devicePath=Join-Path $fixture 'api/luxmap/devices/LUXMAP-P001'
New-Item -ItemType Directory (Split-Path $devicePath) -Force | Out-Null
function Write-Fixture([double[]]$currents,[double]$voltage=5) {
    $now=[DateTimeOffset]::UtcNow
    $rows=for($i=0;$i -lt $currents.Count;$i++){
        @{receivedAt=$now.AddSeconds(($i+1-$currents.Count)*10).ToString('O');data=@{
            lampCommand='ON';voltage=$voltage;current=$currents[$i];power=$voltage*$currents[$i];sessionId='demo';sequenceNumber=$i+1}}
    }
    $latest=$rows[-1]
    @{deviceId='LUXMAP-P001';latestTelemetry=$latest;history=@($rows)} |
        ConvertTo-Json -Depth 8 -Compress | Set-Content $devicePath
}
$fixtureJob=Start-Job -ArgumentList $devicePath -ScriptBlock {
    param($path)
    $listener=[Net.HttpListener]::new()
    $listener.Prefixes.Add('http://127.0.0.1:15152/')
    $listener.Start()
    'ready'
    try {
        while ($listener.IsListening) {
            $ctx=$listener.GetContext()
            try {
                $bytes=[IO.File]::ReadAllBytes($path)
                $ctx.Response.ContentType='application/json'
                $ctx.Response.ContentLength64=$bytes.Length
                $ctx.Response.OutputStream.Write($bytes,0,$bytes.Length)
            } finally {$ctx.Response.Close()}
        }
    } finally {$listener.Close()}
}
Set-Item -LiteralPath 'Env:IotDiagnostics__Servers__iot-server-01__BaseUrl' -Value 'http://127.0.0.1:15152'
$env:IotDiagnostics__BaseUrl='http://127.0.0.1:15152'
$root='http://127.0.0.1:5144'
$dll=Join-Path $out 'LuxMap.Api.dll'
$backend=Start-Process dotnet -ArgumentList ('"'+$dll+'" --environment Development --contentRoot "'+$out+'" --urls '+$root) -WorkingDirectory $repo -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $fixture 'app.log') -RedirectStandardError (Join-Path $fixture 'app.err')
try {
    $fixtureReady=$false
    for($i=0;$i -lt 16;$i++){
        if((Receive-Job $fixtureJob) -contains 'ready'){$fixtureReady=$true;break}
        Start-Sleep -Milliseconds 500
    }
    if(!$fixtureReady){throw 'Fixture HTTP server not ready'}
    $ready=$false
    for($i=0;$i -lt 22;$i++){
        if($backend.HasExited){Get-Content (Join-Path $fixture 'app.log') -Tail 15;throw 'Backend exited'}
        try{Invoke-WebRequest "$root/swagger/v1/swagger.json" -TimeoutSec 2|Out-Null;$ready=$true;break}catch{Start-Sleep -Seconds 1}
    }
    if(!$ready){throw 'Swagger unavailable'}
    $line=Get-Content (Join-Path $repo '.env')|Where-Object {$_ -match '^SEED_ENGINEER_PASSWORD='}|Select-Object -First 1
    $password=($line -split '=',2)[1].Trim().Trim([char]34,[char]39)
    $auth=Invoke-RestMethod "$root/api/v1/auth/login" -Method Post -ContentType 'application/json' -Body (@{username='engineer';password=$password}|ConvertTo-Json -Compress)
    $headers=@{Authorization="Bearer $($auth.access_token)"}
    function Read-Diagnosis {Invoke-RestMethod "$root/api/v1/iot/nodes/NODE-001/diagnostics" -Headers $headers -TimeoutSec 10}
    Write-Fixture ((@(77.5)*22)+@(85,70,115))
    $r=Read-Diagnosis
    if($r.lamp_count.estimated_missing -ne 2 -or $r.lamp_count.votes.'8' -ne 22 -or $r.lamp_count.ambiguous_samples -ne 1 -or $r.lamp_count.outside_range_samples -ne 1 -or $r.anomalies.code -notcontains 'lamp_count_shortfall'){ $r|ConvertTo-Json -Depth 8;throw 'Unexpected lamp count' }
    '25 samples: 22 votes for 8, 1 ambiguous, 1 outside, 2 missing.'
    Write-Fixture (@(95)*25)
    $r=Read-Diagnosis
    if($r.lamp_count.status -ne 'undetermined' -or $r.lamp_count.ambiguous_samples -ne 25){throw 'Expected undetermined'}
    '25 overlapping samples: undetermined.'
    Write-Fixture (@(77.5)*24)
    $r=Read-Diagnosis
    if($r.lamp_count.status -ne 'insufficient_samples' -or $r.lamp_count.samples -ne 24){throw 'Expected 24 samples'}
    '24 samples: insufficient_samples.'
    Write-Fixture (@(0)*25) 0.05
    $r=Read-Diagnosis
    if($r.anomalies.code -notcontains 'voltage_loss' -or $null -ne $r.lamp_count){throw 'Expected voltage loss only'}
    '0.05 V: voltage_loss, no lamp count.'
} finally {
    if($backend -and !$backend.HasExited){Stop-Process -Id $backend.Id}
    Remove-Job $fixtureJob -Force
}
