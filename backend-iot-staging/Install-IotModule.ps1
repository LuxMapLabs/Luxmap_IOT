$ErrorActionPreference='Stop'
$repo='E:\Ki9\DoAn\Github\luxmap_backend'
$stage=$PSScriptRoot
$target=Join-Path $repo 'src/LuxMap.Modules.IOT'
$edits=@(
    @('src/LuxMap.Api/Program.cs','using LuxMap.Modules.Identity;','using LuxMap.Modules.IOT;'),
    @('src/LuxMap.Api/Program.cs','    new TelemetryModule(),','    new IotModule(),'),
    @('src/LuxMap.Api/LuxMap.Api.csproj','    <ProjectReference Include="..\LuxMap.Modules.Telemetry\LuxMap.Modules.Telemetry.csproj" />','    <ProjectReference Include="..\LuxMap.Modules.IOT\LuxMap.Modules.IOT.csproj" />'),
    @('LuxMap.slnx','    <Project Path="src/LuxMap.Modules.Telemetry/LuxMap.Modules.Telemetry.csproj" />','    <Project Path="src/LuxMap.Modules.IOT/LuxMap.Modules.IOT.csproj" />')
)
$updates=@{}
foreach($edit in $edits){
    $path=Join-Path $repo $edit[0]
    $content=if($updates.ContainsKey($path)){$updates[$path]}else{[IO.File]::ReadAllText($path)}
    if(!$content.Contains($edit[1]) -or $content.Contains($edit[2])){throw "Unexpected content in $path"}
    $lineEnd=if($content.Contains([string][char]13)){[Environment]::NewLine}else{[string][char]10}
    $updates[$path]=$content.Replace($edit[1],$edit[1]+$lineEnd+$edit[2])
}
New-Item -ItemType Directory -Path $target | Out-Null
foreach($name in @('IotDiagnosticsController.cs','IotDiagnosticsService.cs','IotModule.cs','LuxMap.Modules.IOT.csproj')){
    Copy-Item -LiteralPath (Join-Path $stage $name) -Destination $target
}
foreach($path in $updates.Keys){[IO.File]::WriteAllText($path,$updates[$path])}
dotnet build (Join-Path $repo 'src/LuxMap.Api/LuxMap.Api.csproj') --disable-build-servers
if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)." }
