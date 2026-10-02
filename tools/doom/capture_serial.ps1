param(
    [Parameter(Mandatory = $true)]
    [string]$LogPath,
    [string]$PortName = 'COM6',
    [int]$BaudRate = 9600,
    [int]$Minutes = 15
)

$fullLogPath = [System.IO.Path]::GetFullPath($LogPath)
[System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($fullLogPath)) | Out-Null
$serial = [System.IO.Ports.SerialPort]::new($PortName, $BaudRate)
$serial.ReadTimeout = 200
$writer = [System.IO.StreamWriter]::new($fullLogPath, $true, [System.Text.Encoding]::ASCII)
$writer.AutoFlush = $true

try {
    $serial.Open()
    Write-Output "SERIAL_CAPTURE_STARTED port=$PortName log=$fullLogPath"
    $until = [DateTime]::UtcNow.AddMinutes($Minutes)
    while ([DateTime]::UtcNow -lt $until) {
        $data = $serial.ReadExisting()
        if ($data.Length -gt 0) { $writer.Write($data) }
        Start-Sleep -Milliseconds 100
    }
} finally {
    $writer.Close()
    if ($serial.IsOpen) { $serial.Close() }
    $serial.Dispose()
    Write-Output 'SERIAL_CAPTURE_COMPLETE'
}
