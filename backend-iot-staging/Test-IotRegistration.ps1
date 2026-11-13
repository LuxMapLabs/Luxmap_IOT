$ErrorActionPreference='Stop'
$repo='E:\Ki9\DoAn\Github\luxmap_backend'
$dir=Join-Path $env:TEMP ('luxmap-register-smoke-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $dir | Out-Null
$base='http://127.0.0.1:5143'
$api=Join-Path $repo 'src/LuxMap.Api'
Copy-Item (Join-Path $api 'appsettings.json') $dir
$config=Get-Content (Join-Path $api 'appsettings.Development.json') -Raw|ConvertFrom-Json -AsHashtable
$config.Remove('IotDiagnostics')
$configFile=Join-Path $dir 'appsettings.Development.json'
$config|ConvertTo-Json -Depth 20|Set-Content $configFile
$dll=Join-Path $env:TEMP 'luxmap-iot-registration-build/LuxMap.Api.dll'
$out=Join-Path $dir 'app.log'
$err=Join-Path $dir 'app.err'
$p=Start-Process dotnet -ArgumentList ('"'+$dll+'" --environment Development --contentRoot "'+$dir+'" --urls '+$base) -WorkingDirectory $repo -WindowStyle Hidden -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
try{
    $ready=$false
    for($i=0;$i -lt 22;$i++){
        if($p.HasExited){Get-Content $out,$err -Tail 15;throw 'Backend exited'}
        try{Invoke-WebRequest "$base/swagger/v1/swagger.json" -TimeoutSec 2|Out-Null;$ready=$true;break}catch{Start-Sleep -Seconds 1}
    }
    if(!$ready){throw 'Backend not ready'}
    function Login($user,$key){
        $line=Get-Content (Join-Path $repo '.env') | Where-Object {$_ -match "^$key="} | Select-Object -First 1
        $pw=($line -split '=',2)[1].Trim().Trim([char]34,[char]39)
        $t=Invoke-RestMethod "$base/api/v1/auth/login" -Method Post -ContentType 'application/json' -Body (@{username=$user;password=$pw}|ConvertTo-Json -Compress)
        return @{Authorization="Bearer $($t.access_token)"}
    }
    function Request($headers,$verb,$path,$data,$expected){
        $args=@{Uri="$base/api/v1/$path";Headers=$headers;Method=$verb;SkipHttpErrorCheck=$true;TimeoutSec=10}
        if($null -ne $data){$args.ContentType='application/json';$args.Body=$data|ConvertTo-Json -Compress}
        $r=Invoke-WebRequest @args
        "$verb $path HTTP $([int]$r.StatusCode)"
        if([int]$r.StatusCode -ne $expected){$r.Content;throw "Expected $expected"}
    }
    $admin=Login admin SEED_ADMIN_PASSWORD
    $manager=Login engineer SEED_ENGINEER_PASSWORD
    $server=@{server_id='smoke-iot';base_url='http://127.0.0.1:5000'}
    Request $manager Post 'iot/servers' $server 403
    Request $admin Post 'iot/servers' $server 201
    Request $admin Post 'iot/servers' $server 409
    $profile=@{server_id='smoke-iot';device_id='LUXMAP-P001';feeder_id='FDR-001';pole_id='POLE-0001';nominal_voltage=5;nominal_current_ma=740;nominal_power_mw=3900}
    Request $admin Post 'iot/nodes/NODE-001/source' $profile 201
    $listed=Invoke-RestMethod "$base/api/v1/iot/nodes" -Headers $manager
    if(($listed.items|Where-Object node_id -eq 'NODE-001').server_id -ne 'smoke-iot'){throw 'Mapping not visible to existing reader'}
    $saved=Get-Content $configFile -Raw|ConvertFrom-Json
    if($saved.IotDiagnostics.Nodes.'NODE-001'.DeviceId -ne 'LUXMAP-P001'){throw 'Mapping not persisted'}
    'Existing node API sees new mapping; file persists it.'
} finally {if(!$p.HasExited){Stop-Process -Id $p.Id}}
