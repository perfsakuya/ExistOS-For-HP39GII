# Rebuilding the embedded E1M1 asset

1. Obtain Freedoom release 0.13.0 from
   <https://github.com/freedoom/freedoom/releases/tag/v0.13.0> and verify
   the official release checksum before extraction.
2. Run `python trim_e1m1.py freedoom1.wad freedoom-e1m1.wad`.
3. Run upstream GBADoom `GbaWadUtil.exe -in freedoom-e1m1.wad -cfile
   freedoom-e1m1-gba.c`.
4. Run `python c_array_to_bin.py freedoom-e1m1-gba.c
   freedoom-e1m1-gba.wad` and place the binary in
   `System/applications/user/doom_port/data/`.

The checked-in converted WAD is 7,025,444 bytes, SHA-256
`88403318CB328E42135BBE81607234F5CB781D6D95FB17A3399FD3084EC6943E`.
Retain Freedoom's `COPYING.txt` and `CREDITS.txt` with any redistributed
output. The intermediate C array is large and should not be checked in.

## Live serial debugging

The calculator exposes a 9600-baud USB serial port while System is running.
Start the capture and dashboard in separate PowerShell terminals from the
repository root:

```powershell
pwsh -File tools/doom/capture_serial.ps1 -PortName COM6 -LogPath .\doom-serial.log
python tools/doom/serial_dashboard.py --log .\doom-serial.log --serial-name COM6
```

Open <http://127.0.0.1:8765/> to watch System memory, ZRAM, Doom startup,
swap-zone verification, frame progress, errors, and the raw log. The dashboard
only reads the file and listens on localhost. Stop the serial capture before
running `edb`, because both need the device's USB interface.
