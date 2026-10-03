param(
    [string]$Repo = "C:\microDOS",
    [string]$ConsolePort = "COM3",
    [string]$ResetPort = "COM8",
    [int]$CaptureSeconds = 15,
    [int]$UsbSettleMs = 1500,
    [int]$RpiBootRetries = 3,
    [switch]$NoBuild,
    [switch]$ExternalConsole
)

$ErrorActionPreference = "Stop"

$Repo = (Resolve-Path $Repo).Path
$BuildScript = Join-Path $Repo "md_pi0w_baremetal.ps1"
$BuiltKernel = Join-Path $Repo "build-pi0w\out\kernel8.img"
$UsbDir = Join-Path $Repo "build-pi0w\usbboot"
$UsbKernel = Join-Path $UsbDir "kernel8.img"
$RpiBoot = "C:\Program Files (x86)\Raspberry Pi\rpiboot.exe"

if (-not (Test-Path $RpiBoot)) {
    throw "rpiboot.exe not found: $RpiBoot"
}

if (-not $NoBuild) {
    Write-Host ""
    Write-Host "=== BUILD ==="
    & $BuildScript build
    if ($LASTEXITCODE -ne 0) {
        throw "microDOS build failed with exit code $LASTEXITCODE"
    }
}

if (-not (Test-Path $BuiltKernel)) {
    throw "Built kernel not found: $BuiltKernel"
}

Write-Host ""
Write-Host "=== STAGE ==="
Copy-Item $BuiltKernel $UsbKernel -Force

$kernel = Get-Item $UsbKernel
Write-Host ("kernel8.img: {0} bytes  {1}" -f $kernel.Length, $kernel.LastWriteTime)

# A stale rpiboot instance can consume the wrong USB stage.
Get-Process rpiboot -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 250

Write-Host ""
Write-Host "=== RESET + USB BOOT + UART ==="
if ($ExternalConsole) {
    Write-Host "COM3 capture: external"
} else {
    Write-Host "COM3 capture: this window"
}

$external = if ($ExternalConsole) { "1" } else { "0" }

$py = @'
import os
import subprocess
import sys
import threading
import time

import serial

console_port = os.environ["MD_CONSOLE_PORT"]
reset_port = os.environ["MD_RESET_PORT"]
rpiboot = os.environ["MD_RPIBOOT"]
usbdir = os.environ["MD_USBDIR"]
capture_seconds = float(os.environ["MD_CAPTURE_SECONDS"])
usb_settle = float(os.environ["MD_USB_SETTLE_MS"]) / 1000.0
retries = int(os.environ["MD_RPIBOOT_RETRIES"])
external_console = os.environ["MD_EXTERNAL_CONSOLE"] == "1"

console = None

def pulse_reset():
    print(f"[reset] pulsing Pi RUN through {reset_port}", flush=True)
    reset = serial.Serial(reset_port, 115200, timeout=0.5)
    try:
        time.sleep(0.20)
        reset.reset_input_buffer()
        reset.write(b"R")
        reset.flush()
        time.sleep(0.40)
        reply = reset.read(4096)
        if reply:
            text = reply.decode(errors="replace").rstrip()
            if text:
                print(text, flush=True)
    finally:
        reset.close()

def run_rpiboot(attempt):
    print(f"[usb] starting fresh one-shot rpiboot (attempt {attempt}/{retries})", flush=True)
    proc = subprocess.Popen(
        [rpiboot, "-d", usbdir, "-v"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )

    lines = []

    def usb_reader():
        assert proc.stdout is not None
        for line in proc.stdout:
            lines.append(line)
            print("[usb] " + line, end="", flush=True)

    thread = threading.Thread(target=usb_reader, daemon=True)
    thread.start()
    return proc, thread, lines

try:
    if not external_console:
        print(f"[uart] opening {console_port} before reset", flush=True)
        console = serial.Serial(console_port, 115200, timeout=0.05)
        console.reset_input_buffer()

    # Fresh hardware reset puts the Pi back in USB ROM boot.
    pulse_reset()

    print(f"[usb] waiting {usb_settle:.2f}s for Windows USB enumeration", flush=True)
    time.sleep(usb_settle)

    proc = None
    thread = None
    lines = None
    success = False

    for attempt in range(1, retries + 1):
        proc, thread, lines = run_rpiboot(attempt)

        # While rpiboot is running, continue collecting UART so early kernel
        # output is never lost.
        while proc.poll() is None:
            if console is not None:
                try:
                    data = console.read(4096)
                    if data:
                        print(data.decode(errors="replace"), end="", flush=True)
                except (serial.SerialException, OSError) as exc:
                    print(f"\n[uart] disconnected: {exc}", flush=True)
                    try:
                        console.close()
                    except Exception:
                        pass
                    console = None
            time.sleep(0.005)

        rc = proc.returncode
        thread.join(timeout=1.0)
        print(f"\n[usb] rpiboot exited: {rc}", flush=True)

        if rc == 0:
            success = True
            break

        # Common Windows race: device exists but descriptors are not readable
        # yet. Do not force another Pi reset first; the ROM device normally
        # remains waiting for rpiboot. Give Plug-and-Play time to settle and retry.
        if attempt < retries:
            print("[usb] transient failure; waiting 1.0s and retrying without reset", flush=True)
            time.sleep(1.0)

    if not success:
        print("[usb] retries exhausted; doing one fresh hardware reset and final attempt", flush=True)
        pulse_reset()
        time.sleep(max(usb_settle, 1.5))
        proc, thread, lines = run_rpiboot(retries + 1)

        while proc.poll() is None:
            if console is not None:
                try:
                    data = console.read(4096)
                    if data:
                        print(data.decode(errors="replace"), end="", flush=True)
                except (serial.SerialException, OSError):
                    try:
                        console.close()
                    except Exception:
                        pass
                    console = None
            time.sleep(0.005)

        thread.join(timeout=1.0)
        print(f"\n[usb] rpiboot exited: {proc.returncode}", flush=True)
        success = (proc.returncode == 0)

    if not success:
        raise SystemExit(2)

    if not external_console:
        print(f"[uart] rpiboot complete; capturing for {capture_seconds:.0f}s", flush=True)

        deadline = time.monotonic() + capture_seconds
        while time.monotonic() < deadline:
            if console is not None:
                try:
                    data = console.read(4096)
                    if data:
                        print(data.decode(errors="replace"), end="", flush=True)
                except (serial.SerialException, OSError) as exc:
                    print(f"\n[uart] disconnected: {exc}", flush=True)
                    try:
                        console.close()
                    except Exception:
                        pass
                    console = None
            time.sleep(0.005)

finally:
    if console is not None:
        try:
            console.close()
        except Exception:
            pass

print("\n=== RUN COMPLETE ===", flush=True)
'@

$env:MD_CONSOLE_PORT = $ConsolePort
$env:MD_RESET_PORT = $ResetPort
$env:MD_RPIBOOT = $RpiBoot
$env:MD_USBDIR = $UsbDir
$env:MD_CAPTURE_SECONDS = [string]$CaptureSeconds
$env:MD_USB_SETTLE_MS = [string]$UsbSettleMs
$env:MD_RPIBOOT_RETRIES = [string]$RpiBootRetries
$env:MD_EXTERNAL_CONSOLE = $external

try {
    $py | python -
    if ($LASTEXITCODE -ne 0) {
        throw "runner failed with exit code $LASTEXITCODE"
    }
}
finally {
    Remove-Item Env:MD_CONSOLE_PORT -ErrorAction SilentlyContinue
    Remove-Item Env:MD_RESET_PORT -ErrorAction SilentlyContinue
    Remove-Item Env:MD_RPIBOOT -ErrorAction SilentlyContinue
    Remove-Item Env:MD_USBDIR -ErrorAction SilentlyContinue
    Remove-Item Env:MD_CAPTURE_SECONDS -ErrorAction SilentlyContinue
    Remove-Item Env:MD_USB_SETTLE_MS -ErrorAction SilentlyContinue
    Remove-Item Env:MD_RPIBOOT_RETRIES -ErrorAction SilentlyContinue
    Remove-Item Env:MD_EXTERNAL_CONSOLE -ErrorAction SilentlyContinue
}
