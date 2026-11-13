using System.Diagnostics;
using System.Net.Http.Json;
using System.Text.Json;
using System.Threading.Channels;
using MQTTnet;
using MQTTnet.Client;
using MQTTnet.Formatter;
using MQTTnet.Protocol;

// Gateway simulation. Separate ports/data; never controls the user's real lamp.
var serverDll = Path.GetFullPath(args[0]);
var root = Path.Combine(Path.GetTempPath(), "luxmap-gateway-test-" + Guid.NewGuid().ToString("N"));
Directory.CreateDirectory(root);
using var http = new HttpClient { BaseAddress = new Uri("http://127.0.0.1:15001"), Timeout = TimeSpan.FromSeconds(5) };
Process? server = null;
var output = new List<string>();
const string id = "LUXMAP-P001", gateway = "LUXMAP-GW001";
var commands = Channel.CreateUnbounded<string>();
var acknowledgements = Channel.CreateUnbounded<string>();
var factory = new MqttFactory();
using var client = factory.CreateMqttClient();
client.ApplicationMessageReceivedAsync += e =>
{
    var channel = e.ApplicationMessage.Topic.EndsWith("/command") ? commands : acknowledgements;
    channel.Writer.TryWrite(e.ApplicationMessage.ConvertPayloadToString());
    return Task.CompletedTask;
};
var options = new MqttClientOptionsBuilder().WithClientId(gateway).WithProtocolVersion(MqttProtocolVersion.V311)
    .WithTcpServer("127.0.0.1", 11883).Build();
async Task StartServer()
{
    var info = new ProcessStartInfo("dotnet") { UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true, CreateNoWindow = true };
    foreach (var argument in new[] { serverDll, "--urls", "http://127.0.0.1:15001", "--Mqtt:Port", "11883", "--contentRoot", root }) info.ArgumentList.Add(argument);
    server = Process.Start(info)!;
    server.OutputDataReceived += (_, e) => { if (e.Data is { } line) lock (output) output.Add(line); };
    server.ErrorDataReceived += (_, e) => { if (e.Data is { } line) lock (output) output.Add(line); };
    server.BeginOutputReadLine(); server.BeginErrorReadLine();
    for (var attempt = 0; attempt < 50; attempt++)
    {
        if (server.HasExited) throw new Exception("Test server exited");
        try { if ((await http.GetAsync("/api/luxmap/mqtt")).IsSuccessStatusCode) return; } catch (HttpRequestException) { }
        await Task.Delay(100);
    }
    throw new Exception("Server readiness timeout");
}
async Task StopServer()
{
    if (server is null) return;
    if (!server.HasExited) { server.Kill(entireProcessTree: true); await server.WaitForExitAsync(); }
    server.Dispose(); server = null;
}
async Task Connect()
{
    await client.ConnectAsync(options);
    await client.SubscribeAsync(factory.CreateSubscribeOptionsBuilder()
        .WithTopicFilter(filter => filter.WithTopic($"luxmap/{id}/command").WithAtLeastOnceQoS())
        .WithTopicFilter(filter => filter.WithTopic($"luxmap/{id}/telemetryAck").WithAtLeastOnceQoS()).Build());
}
async Task<JsonElement> Read(Channel<string> channel)
{
    using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(5));
    using var doc = JsonDocument.Parse(await channel.Reader.ReadAsync(timeout.Token));
    return doc.RootElement.Clone();
}
async Task<JsonElement> ExpectCommand(string mode, string? command)
{
    var value = await Read(commands);
    if (value.GetProperty("mode").GetString() != mode || value.GetProperty("command").GetString() != command ||
        value.GetProperty("deviceId").GetString() != id) throw new Exception("Wrong routed command");
    return value;
}
async Task Publish(string kind, object value)
{
    await client.PublishAsync(new MqttApplicationMessageBuilder().WithTopic($"luxmap/{id}/{kind}")
        .WithPayload(JsonSerializer.Serialize(value)).WithQualityOfServiceLevel(MqttQualityOfServiceLevel.AtLeastOnce).Build());
}
async Task<JsonElement> State()
{
    using var doc = JsonDocument.Parse(await http.GetStringAsync($"/api/luxmap/devices/{id}"));
    return doc.RootElement.Clone();
}
async Task SendTelemetry(int sequence, bool duplicate)
{
    await Publish("telemetry", new { kind = "telemetry", deviceId = id, sessionId = "BOOT-A", sequenceNumber = sequence,
        mode = "MANUAL", lampCommand = "OFF", lux = 25.5, voltage = 12.1, current = 0.0, power = 0.0 });
    var ack = await Read(acknowledgements);
    if (ack.GetProperty("deviceId").GetString() != id || ack.GetProperty("sessionId").GetString() != "BOOT-A" ||
        ack.GetProperty("sequenceNumber").GetInt32() != sequence || ack.GetProperty("duplicate").GetBoolean() != duplicate)
        throw new Exception("Incorrect server storage ACK");
}
try
{
    await StartServer();
    if (!(await http.GetStringAsync("/swagger/v1/swagger.json")).Contains("/command")) throw new Exception("Swagger missing");
    await Connect(); await ExpectCommand("AUTO", null);
    JsonElement last = default;
    foreach (var pair in new[] { ("MANUAL", (string?)"ON"), ("AUTO", (string?)null), ("MANUAL", (string?)"OFF") })
    {
        var response = await http.PostAsJsonAsync($"/api/luxmap/devices/{id}/command", new { mode = pair.Item1, command = pair.Item2 });
        response.EnsureSuccessStatusCode();
        using var posted = JsonDocument.Parse(await response.Content.ReadAsStringAsync());
        last = await ExpectCommand(pair.Item1, pair.Item2);
        if (last.GetProperty("commandId").GetString() != posted.RootElement.GetProperty("commandId").GetString())
            throw new Exception("HTTP/MQTT command correlation mismatch");
    }
    if ((int)(await http.PostAsJsonAsync($"/api/luxmap/devices/{id}/command", new { mode = "MANUAL" })).StatusCode != 400)
        throw new Exception("Invalid command accepted");
    if ((int)(await http.PostAsJsonAsync("/api/luxmap/devices/UNASSIGNED/command", new { mode = "AUTO" })).StatusCode != 400)
        throw new Exception("Unassigned node accepted");
    await SendTelemetry(2, false); await SendTelemetry(1, false); await SendTelemetry(2, true);
    var state = await State();
    if (state.GetProperty("history").GetArrayLength() != 2 ||
        state.GetProperty("latestTelemetry").GetProperty("data").GetProperty("sequenceNumber").GetInt32() != 2)
        throw new Exception("Out-of-order dedup/latest failed");
    if (state.GetProperty("commandAcknowledged").GetBoolean()) throw new Exception("Command marked delivered before node ACK");
    await Publish("commandAck", new { deviceId = id, commandId = "wrong", mode = "MANUAL", lampCommand = "OFF" });
    if ((await State()).GetProperty("commandAcknowledged").GetBoolean()) throw new Exception("Wrong ACK accepted");
    await Publish("commandAck", new { deviceId = id, commandId = last.GetProperty("commandId").GetString(), mode = "MANUAL", lampCommand = "OFF" });
    for (var i = 0; !(await State()).GetProperty("commandAcknowledged").GetBoolean(); i++)
    { if (i > 20) throw new Exception("Node command ACK missing"); await Task.Delay(50); }
    await Publish("telemetry", new { deviceId = "OTHER", sessionId = "BOOT-A", sequenceNumber = 3 });
    var denied = await client.SubscribeAsync(factory.CreateSubscribeOptionsBuilder().WithTopicFilter("luxmap/+/command").Build());
    if ((int)denied.Items.First().ResultCode < 128) throw new Exception("Wildcard subscription accepted");
    await Publish("command", new { mode = "MANUAL", command = "ON" });
    await client.DisconnectAsync(); await Connect(); await ExpectCommand("MANUAL", "OFF");
    await client.DisconnectAsync(); await StopServer(); await StartServer();
    await Connect(); await ExpectCommand("MANUAL", "OFF"); await SendTelemetry(1, true);
    state = await State();
    if (state.GetProperty("history").GetArrayLength() != 2 || !state.GetProperty("commandAcknowledged").GetBoolean())
        throw new Exception("Persistence failed");
    if (acknowledgements.Reader.TryRead(out _)) throw new Exception("Invalid telemetry was ACKed");
    await client.DisconnectAsync();
    Console.WriteLine("PASS: gateway MQTT routing, Swagger ON/OFF/AUTO, durable ACK, duplicate/out-of-order replay, node command ACK, invalid topic/body rejection, reconnect + restart persistence");
}
catch { lock (output) Console.Error.WriteLine(string.Join("\n", output.TakeLast(50))); throw; }
finally { await StopServer(); }
