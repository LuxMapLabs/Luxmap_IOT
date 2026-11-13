$ErrorActionPreference='Stop'
$repo='E:\Ki9\DoAn\Github\luxmap_backend'
$api=Join-Path $repo 'src/LuxMap.Api'
$proto='E:\Ki9\DoAn\iot\code iot\server'
docker compose --project-directory $repo ps postgres
if($LASTEXITCODE){exit $LASTEXITCODE}
$root='http://127.0.0.1:5142'
$backendDll=Join-Path $env:TEMP 'luxmap-iot-fleet-build/LuxMap.Api.dll'
try{Invoke-WebRequest 'http://127.0.0.1:5000/api/luxmap/mqtt' -TimeoutSec 2 | Out-Null}
catch{
    $p=Start-Process dotnet -ArgumentList ('"'+(Join-Path $proto 'bin/Debug/net8.0/LuxMap.Api.dll')+'"') -WorkingDirectory $proto -WindowStyle Hidden -PassThru
    Start-Sleep -Seconds 3
    if($p.HasExited){throw 'IoT source did not start.'}
}
$log=Join-Path $env:TEMP 'luxmap-iot-fleet.log'
$err=Join-Path $env:TEMP 'luxmap-iot-fleet.err'
$p=Start-Process dotnet -ArgumentList ('"'+$backendDll+'" --environment Development --urls '+$root) -WorkingDirectory $api -WindowStyle Hidden -PassThru -RedirectStandardOutput $log -RedirectStandardError $err
$ready=$false
for($i=0;$i -lt 22;$i++){
    if($p.HasExited){Get-Content $log,$err -Tail 12;throw 'Backend exited.'}
    try{$swagger=Invoke-RestMethod "$root/swagger/v1/swagger.json" -TimeoutSec 2;$ready=$true;break}catch{Start-Sleep -Seconds 1}
}
if(!$ready){throw 'Swagger unavailable.'}
$line=Get-Content (Join-Path $repo '.env') | Where-Object {$_ -match '^SEED_ENGINEER_PASSWORD='} | Select-Object -First 1
$pw=($line -split '=',2)[1].Trim().Trim([char]34,[char]39)
$login=Invoke-RestMethod "$root/api/v1/auth/login" -Method Post -ContentType 'application/json' -Body (@{username='engineer';password=$pw}|ConvertTo-Json -Compress)
foreach($route in @('servers','nodes','nodes/NODE-001/electrical','nodes/NODE-001/diagnostics')){
    $r=Invoke-WebRequest "$root/api/v1/iot/$route" -Headers @{Authorization="Bearer $($login.access_token)"} -TimeoutSec 20
    "$route HTTP $([int]$r.StatusCode)"
    $r.Content
}
