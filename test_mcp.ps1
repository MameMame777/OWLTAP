param([string]$ServerHost = "127.0.0.1", [int]$Port = 4711)

$ErrorActionPreference = "Stop"

try {
    $tcp = [System.Net.Sockets.TcpClient]::new($ServerHost, $Port)
} catch {
    Write-Error "Cannot connect to ${ServerHost}:${Port} — $_"
    exit 1
}

$stream = $tcp.GetStream()
$stream.ReadTimeout  = 5000
$stream.WriteTimeout = 5000

function Send-Receive([string]$json) {
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($json)
    $header = "Content-Length: $($bytes.Length)`r`n`r`n"
    $hbytes = [System.Text.Encoding]::ASCII.GetBytes($header)
    $stream.Write($hbytes, 0, $hbytes.Length)
    $stream.Write($bytes,  0, $bytes.Length)
    $stream.Flush()

    # Read response header
    $sb = [System.Text.StringBuilder]::new()
    $prev = 0
    while ($true) {
        $b = $stream.ReadByte()
        if ($b -lt 0) { throw "Connection closed" }
        if ($prev -eq 13 -and $b -eq 10) { break }  # \r\n
        if ($b -ne 13) { $null = $sb.Append([char]$b) }
        $prev = $b
    }
    # blank line
    $stream.ReadByte() | Out-Null  # \r
    $stream.ReadByte() | Out-Null  # \n

    $headerLine = $sb.ToString()
    $len = [int]($headerLine -replace 'Content-Length:\s*', '')
    $rbuf = New-Object byte[] $len
    $read = 0
    while ($read -lt $len) {
        $n = $stream.Read($rbuf, $read, $len - $read)
        if ($n -le 0) { throw "Stream ended early" }
        $read += $n
    }
    return [System.Text.Encoding]::UTF8.GetString($rbuf)
}

# ── initialize ──────────────────────────────────────────────────────
Write-Host "`n=== initialize ===" -ForegroundColor Cyan
$r = Send-Receive '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"pwsh-test","version":"0.1"}}}'
$r | ConvertFrom-Json | ConvertTo-Json -Depth 4

# ── tools/list ──────────────────────────────────────────────────────
Write-Host "`n=== tools/list ===" -ForegroundColor Cyan
$r = Send-Receive '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}'
$obj = $r | ConvertFrom-Json
$obj.result.tools | ForEach-Object { "  - $($_.name)" } | Write-Host

# ── read_idcode ─────────────────────────────────────────────────────
Write-Host "`n=== read_idcode ===" -ForegroundColor Cyan
$r = Send-Receive '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"read_idcode","arguments":{}}}'
$r | ConvertFrom-Json | ConvertTo-Json -Depth 6

$tcp.Close()
Write-Host "`nDone." -ForegroundColor Green
