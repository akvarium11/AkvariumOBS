param(
    [Parameter(Mandatory = $true)][string]$Action,
    [string]$Arg1 = "",
    [string]$Arg2 = ""
)

$ErrorActionPreference = "Stop"
$uri = [Uri]"ws://127.0.0.1:4455"
$ws = New-Object System.Net.WebSockets.ClientWebSocket
$ct = [System.Threading.CancellationToken]::None
$ws.ConnectAsync($uri, $ct).Wait()

function Send-Json($obj) {
    $json = $obj | ConvertTo-Json -Depth 12 -Compress
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($json)
    $seg = New-Object System.ArraySegment[byte] -ArgumentList @(,$bytes)
    $ws.SendAsync($seg, [System.Net.WebSockets.WebSocketMessageType]::Text, $true, $ct).Wait()
}

function Receive-Json {
    $buffer = New-Object byte[] 1048576
    $seg = New-Object System.ArraySegment[byte] -ArgumentList @(,$buffer)
    $sb = New-Object System.Text.StringBuilder
    do {
        $res = $ws.ReceiveAsync($seg, $ct).Result
        [void]$sb.Append([System.Text.Encoding]::UTF8.GetString($buffer, 0, $res.Count))
    } while (-not $res.EndOfMessage)
    return ($sb.ToString() | ConvertFrom-Json)
}

$hello = Receive-Json
Send-Json @{ op = 1; d = @{ rpcVersion = 1 } }
$identified = Receive-Json

$id = [guid]::NewGuid().ToString()
switch ($Action) {
    "create-scene" { Send-Json @{ op = 6; d = @{ requestType = "CreateScene"; requestId = $id; requestData = @{ sceneName = $Arg1 } } } }
    "set-scene" { Send-Json @{ op = 6; d = @{ requestType = "SetCurrentProgramScene"; requestId = $id; requestData = @{ sceneName = $Arg1 } } } }
    "mute" { Send-Json @{ op = 6; d = @{ requestType = "SetInputMute"; requestId = $id; requestData = @{ inputName = $Arg1; inputMuted = ($Arg2 -eq "true") } } } }
    "get-mute" { Send-Json @{ op = 6; d = @{ requestType = "GetInputMute"; requestId = $id; requestData = @{ inputName = $Arg1 } } } }
    "scene-list" { Send-Json @{ op = 6; d = @{ requestType = "GetSceneList"; requestId = $id; requestData = @{} } } }
    "current-scene" { Send-Json @{ op = 6; d = @{ requestType = "GetCurrentProgramScene"; requestId = $id; requestData = @{} } } }
    "inputs" { Send-Json @{ op = 6; d = @{ requestType = "GetInputList"; requestId = $id; requestData = @{} } } }
    "create-input" { Send-Json @{ op = 6; d = @{ requestType = "CreateInput"; requestId = $id; requestData = @{ sceneName = $Arg1; inputName = $Arg2; inputKind = "wasapi_input_capture"; inputSettings = @{} } } } }
    default { Write-Output "unknown action"; exit 2 }
}

$resp = Receive-Json
$resp | ConvertTo-Json -Depth 12 -Compress
$ws.Dispose()
