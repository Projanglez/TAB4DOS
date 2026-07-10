# run.ps1 - Launch the TAB4DOS test VM in QEMU (setup mirrored from ftpcom).
#
#   .\run.ps1                 # normal run: boot hdd.img, D: share + QMP
#   .\run.ps1 -Headless       # no GUI window (screendump still works via QMP)
#
# QMP listens on 127.0.0.1:4446 -> drive it with vmctl.py ($env:QMP_PORT=4446).
param(
    [switch]$Headless
)

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot           # so QEMU's fat: share path stays relative

$qemu = "C:\Program Files\qemu\qemu-system-i386.exe"
$hdd  = Join-Path $PSScriptRoot "hdd.img"

$args = @(
    "-m", "16",
    "-rtc", "base=localtime",
    "-qmp", "tcp:127.0.0.1:4446,server,nowait"
)

if ($Headless) { $args += @("-display", "none") } else { $args += @("-display", "sdl") }

# Boot from HDD; VVFAT share as second disk (D:, guest READS only). NIC kept
# identical to the ftpcom VM so the packet-driver line in AUTOEXEC.BAT finds
# its hardware and the boot flow stays the proven one.
$args += @(
    "-drive", "file=$hdd,format=raw,if=ide,index=0,media=disk",
    "-drive", "file=fat:rw:share,if=ide,index=1",
    "-netdev", "user,id=net0",
    "-device", "ne2k_isa,netdev=net0,iobase=0x300,irq=10",
    "-boot", "c"
)

Write-Host "Launching QEMU (QMP port 4446):" ($args -join " ")
& $qemu @args
