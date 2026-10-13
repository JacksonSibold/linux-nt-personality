# Sends lines to the guest serial console (TCP 127.0.0.1:5555) and prints the output produced.
# Usage: powershell -File serial-cmd.ps1 -InFile <file with one command per line> [-WaitMs 2000]
param(
    [string]$InFile = '',
    [int]$WaitMs = 2000,
    [int]$Port = 5555,
    [int]$ReadSeconds = 4
)
$Lines = @()
if ($InFile -ne '') { $Lines = Get-Content -LiteralPath $InFile }
$client = New-Object System.Net.Sockets.TcpClient
$client.Connect('127.0.0.1', $Port)
$stream = $client.GetStream()
$enc = [System.Text.Encoding]::UTF8
function Drain([int]$ms) {
    $end = (Get-Date).AddMilliseconds($ms)
    $buf = New-Object byte[] 65536
    $sb = New-Object System.Text.StringBuilder
    while ((Get-Date) -lt $end) {
        while ($stream.DataAvailable) {
            $n = $stream.Read($buf, 0, $buf.Length)
            if ($n -gt 0) { [void]$sb.Append($enc.GetString($buf, 0, $n)) }
        }
        Start-Sleep -Milliseconds 100
    }
    return $sb.ToString()
}
$out = Drain 500
foreach ($l in $Lines) {
    $bytes = $enc.GetBytes($l + "`n")
    $stream.Write($bytes, 0, $bytes.Length)
    $stream.Flush()
    $out += Drain $WaitMs
}
$out += Drain ($ReadSeconds * 1000)
$client.Close()
$clean = $out -replace "`e\[[0-9;?]*[A-Za-z]", ''
$clean -replace "`r", ''
