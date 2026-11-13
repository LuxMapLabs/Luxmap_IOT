$ErrorActionPreference='Stop'
$repo='E:\Ki9\DoAn\Github\luxmap_backend'
$proto='E:\Ki9\DoAn\iot\code iot\server'
$api=Join-Path $repo 'src/LuxMap.Api'
$apiDll=Join-Path $api 'bin/Debug/net10.0/LuxMap.Api.dll'
$env:ASPNETCORE_ENVIRONMENT='Development'
$base='http://127.0.0.1:5141'
$source='http://127.0.0.1:5000'
Set-Location -LiteralPath $api
dotnet $apiDll --environment Development --seed
if($LASTEXITCODE -ne 0){throw 'Seed failed.'}

function Start-Backend {
    $script:backend=Start-Process dotnet -ArgumentList @($apiDll,'--environment','Development','--urls',$base) -WorkingDirectory $api -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $env:TEMP 'luxmap-iot-backend.out') -RedirectStandardError (Join-Path $env:TEMP 'luxmap-iot-backend.err')
    for($attempt=0;$attempt -lt 30;$attempt++){
        if($backend.HasExited){
            Get-Content (Join-Path $env:TEMP 'luxmap-iot-backend.out') -Tail 20
            throw 'Backend exited.'
        }
        try { Invoke-WebRequest "$base/swagger/v1/swagger.json" -TimeoutSec 2 | Out-Null; return } catch { Start-Sleep -Seconds 1 }
    }
    throw 'Backend Swagger did not respond.'
}
Start-Backend
$secretLine=Get-Content -LiteralPath (Join-Path $repo '.env') | Where-Object {$_ -match '^SEED_ENGINEER_PASSWORD='} | Select-Object -First 1
if(!$secretLine){throw 'Missing engineer seed password.'}
$password=($secretLine -split '=',2)[1].Trim().Trim([char]34,[char]39)
function Request($method,$path,$body){
    $params=@{Uri="$base/api/v1/$path";Method=$method;ContentType='application/json';TimeoutSec=15;Headers=$script:headers}
    if($null -ne $body){$params.Body=ConvertTo-Json -InputObject $body -Compress}
    Invoke-WebRequest @params
}
function Login {
    $script:headers=@{}
    $session=(Request Post 'auth/login' @{username='engineer';password=$password}).Content | ConvertFrom-Json
    $script:headers=@{Authorization="Bearer $($session.access_token)"}
}
function Create($path,$body){
    $response=Request Post $path $body
    return (([string]$response.Headers.Location).TrimEnd('/') -split '/')[-1]
}
Login
$me=(Request Get 'auth/me' $null).Content | ConvertFrom-Json
$commune=($me.communes | Where-Object name -eq 'Commune 01' | Select-Object -First 1).commune_id
if(!$commune -and @($me.commune_ids).Count -eq 1){$commune=$me.commune_ids[0]}
if(!$commune -or $commune -eq '*'){throw 'Demo commune was not identified.'}
$configFile=Join-Path $api 'appsettings.Development.json'
$config=Get-Content -LiteralPath $configFile -Raw | ConvertFrom-Json -AsHashtable
if($config.Contains('IotDiagnostics')){throw 'IotDiagnostics already configured. Inspect existing mapping before creating assets.'}
$cabinet=Create 'assets/cabinets' @{commune_id=$commune;cabinet_name='Testbed LUXMAP-P001';external_ref='TESTBED-P001-CABINET';data_source='calibration_rig';geom_wkt='POINT (106.7 10.78)'}
$feeder=Create 'assets/feeders' @{commune_id=$commune;feeder_name='Testbed LUXMAP-P001';external_ref='TESTBED-P001-FEEDER';cabinet_id=$cabinet;cabinet_source='verified'}
$pole=Create 'assets/poles' @{commune_id=$commune;external_ref='TESTBED-P001-POLE';data_source='calibration_rig';geom_wkt='POINT (106.7 10.7801)';feeder_id=$feeder;feeder_source='verified';note='Testbed LUXMAP-P001. Demo coordinates.'}
$node=Create 'assets/iot-nodes' @{cabinet_id=$cabinet;data_source='calibration_rig';supports_remote_control=$false}
Request Put "assets/iot-nodes/$node/relays/1" @{feeder_id=$feeder} | Out-Null
[ordered]@{commune=$commune;cabinet=$cabinet;node=$node;relay=1;feeder=$feeder;pole=$pole} | ConvertTo-Json
$config.IotDiagnostics=@{BaseUrl=$source;Nodes=@{$node=@{DeviceId='LUXMAP-P001';FeederId=$feeder;PoleId=$pole;NominalVoltage=5.0;NominalCurrentMa=740;NominalPowerMw=3900}}}
[IO.File]::WriteAllText($configFile,(ConvertTo-Json -InputObject $config -Depth 12)+[Environment]::NewLine)
try {Invoke-RestMethod "$source/api/luxmap/devices/LUXMAP-P001" -TimeoutSec 2 | Out-Null}
catch {
    $protoDll=Join-Path $proto 'bin/Debug/net8.0/LuxMap.Api.dll'
    Start-Process dotnet -ArgumentList ('"'+$protoDll+'"') -WorkingDirectory $proto -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $env:TEMP 'luxmap-iot-source.out') -RedirectStandardError (Join-Path $env:TEMP 'luxmap-iot-source.err') | Select-Object Id
    Start-Sleep -Seconds 4
}
Stop-Process -Id $backend.Id
Start-Backend
Login
$result=Request Get "iot/nodes/$node/diagnostics" $null
"HTTP $([int]$result.StatusCode)"
$result.Content
