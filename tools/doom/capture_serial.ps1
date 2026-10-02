param(
    [Parameter(Mandatory = $true)]
    [string]$LogPath,
    [string]$PortName = 'COM6',
    [int]$BaudRate = 9600,
    [int]$Minutes = 15
)

$fullLogPath = [System.IO.Path]::GetFullPath($LogPath)
[System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($fullLogPath)) | Out-Null
$writer = [System.IO.StreamWriter]::new($fullLogPath, $true, [System.Text.Encoding]::ASCII)
$writer.AutoFlush = $true
$serial = $null

try {
    $until = [DateTime]::UtcNow.AddMinutes($Minutes)
    while ([DateTime]::UtcNow -lt $until) {
        if ($null -eq $serial) {
            $serial = [System.IO.Ports.SerialPort]::new($PortName, $BaudRate)
            $serial.ReadTimeout = 200
            try {
                $serial.Open()
                $writer.WriteLine("SERIAL_CONNECTED utc=$([DateTime]::UtcNow.ToString('o'))")
                Write-Output "SERIAL_CAPTURE_CONNECTED port=$PortName log=$fullLogPath"
            } catch {
                $serial.Dispose()
                $serial = $null
                Start-Sleep -Milliseconds 500
                continue
            }
        }
        try {
            $data = $serial.ReadExisting()
            if ($data.Length -gt 0) { $writer.Write($data) }
        } catch {
            $writer.WriteLine("SERIAL_DISCONNECTED utc=$([DateTime]::UtcNow.ToString('o'))")
            $serial.Dispose()
            $serial = $null
            Start-Sleep -Milliseconds 500
            continue
        }
        Start-Sleep -Milliseconds 100
    }
} finally {
    $writer.Close()
    if ($null -ne $serial) { $serial.Dispose() }
    Write-Output 'SERIAL_CAPTURE_COMPLETE'
}
