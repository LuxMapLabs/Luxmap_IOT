$ErrorActionPreference='Stop'
$api='E:\Ki9\DoAn\Github\luxmap_backend\src\LuxMap.Api'
$repo=Split-Path (Split-Path $api)
$listener=Start-Job -ScriptBlock {
    $server=[Net.HttpListener]::new()
    $server.Prefixes.Add('http://127.0.0.1:15151/')
    $server.Start()
    'ready'
    try {
        $ctx=$server.GetContext()
        $doc=@{deviceId='LUXMAP-P001';latestTelemetry=@{receivedAt=[DateTimeOffset]::UtcNow.ToString('O');data=@{lampCommand='OFF';voltage=5.39;current=743;power=3946}}}
        $buf=[Text.Encoding]::UTF8.GetBytes(($doc | ConvertTo-Json -Depth 6 -Compress))
        $ctx.Response.ContentType='application/json'
        $ctx.Response.ContentLength64=$buf.Length
        $ctx.Response.OutputStream.Write($buf,0,$buf.Length)
        $ctx.Response.Close()
    } finally {$server.Close()}
}
try {
    $ready=$false
    for($i=0;$i -lt 15;$i++){if((Receive-Job $listener) -contains 'ready'){$ready=$true;break};Start-Sleep -Milliseconds 500}
    if(!$ready){throw 'Fixture listener did not start.'}
    $env:IotDiagnostics__BaseUrl='http://127.0.0.1:15151'
    $backend=Start-Process dotnet -ArgumentList @((Join-Path $api 'bin/Debug/net10.0/LuxMap.Api.dll'),'--environment','Development','--urls','http://127.0.0.1:5142') -WorkingDirectory $api -WindowStyle Hidden -PassThru
    $root='http://127.0.0.1:5142/api/v1'
    $ready=$false
    for($i=0;$i -lt 25;$i++){try {Invoke-WebRequest 'http://127.0.0.1:5142/swagger/v1/swagger.json' -TimeoutSec 2|Out-Null;$ready=$true;break}catch{Start-Sleep -Seconds 1}}
    if(!$ready){throw 'Test backend did not start.'}
    $line=Get-Content -LiteralPath (Join-Path $repo '.env') | Where-Object {$_ -match '^SEED_ENGINEER_PASSWORD='} | Select-Object -First 1
    $password=($line -split '=',2)[1].Trim().Trim([char]34,[char]39)
    $login=Invoke-RestMethod "$root/auth/login" -Method Post -ContentType 'application/json' -Body (@{username='engineer';password=$password}|ConvertTo-Json -Compress)
    $result=Invoke-RestMethod "$root/iot/nodes/NODE-001/diagnostics" -Headers @{Authorization="Bearer $($login.access_token)"}
    if($result.status -ne 'anomaly' -or $result.anomalies[0].code -ne 'current_when_off'){throw 'Expected current_when_off.'}
    $result | ConvertTo-Json -Depth 6
} finally {
    if($backend -and !$backend.HasExited){Stop-Process -Id $backend.Id}
    Remove-Job $listener -Force
}
