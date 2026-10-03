param(
    [string]$Repo = "C:\microDOS",
    [int]$CaptureSeconds = 15,
    [switch]$NoBuild
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

Get-Process rpiboot -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 250

Write-Host ""
Write-Host "=== V28 HID UART BRIDGE TEST ==="
Write-Host "No CDC/COM port is used. Transport is 64-byte USB HID interrupt reports."

$py = @'
import os
import re
import struct
import subprocess
import threading
import time
import traceback

try:
    import hid
except Exception as exc:
    print(
        "[runner] Python package 'hidapi' is required for v28.",
        flush=True,
    )
    print(
        "[runner] Install once with: python -m pip install hidapi",
        flush=True,
    )
    print(f"[runner] import error: {exc}", flush=True)
    raise SystemExit(4)

VID = 0xCAFE
PID = 0x4028

RESP_PONG = 1
RESP_CLEARED = 2
RESP_HELD = 3
RESP_RELEASED = 4
RESP_STATUS = 5

rpiboot = os.environ["MD_RPIBOOT"]
usbdir = os.environ["MD_USBDIR"]
capture_seconds = float(os.environ["MD_CAPTURE_SECONDS"])

dev = None
captured = bytearray()
control_responses = []


def u32le(b, off):
    return struct.unpack_from("<I", b, off)[0]


def discover_hid(timeout=12.0):
    deadline = time.monotonic() + timeout

    while time.monotonic() < deadline:
        found = hid.enumerate(VID, PID)

        for item in found:
            product = item.get("product_string") or ""

            if "microDOS HID UART bridge v28" in product:
                return item["path"]

        time.sleep(0.25)

    raise RuntimeError("v28 HID bridge not found after 12 seconds")


def open_hid(path):
    d = hid.device()
    d.open_path(path)
    d.set_nonblocking(1)
    return d


def send_command(ch):
    # hidapi write buffer starts with Report ID.
    # This device uses no report IDs, so byte 0 is the required zero ID.
    payload = bytearray(65)
    payload[0] = 0
    payload[1] = ord(ch)

    written = dev.write(bytes(payload))

    if written <= 0:
        raise RuntimeError(f"HID command {ch!r} write failed")


def print_uart(data):
    if not data:
        return

    captured.extend(data)
    print(data.decode(errors="replace"), end="", flush=True)


def pump_hid_once():
    got_any = False

    while True:
        raw = dev.read(64)

        if not raw:
            break

        got_any = True
        frame = bytes(raw)

        if len(frame) < 2:
            continue

        ftype = frame[0]

        if ftype == 0x01:
            n = min(frame[1], 62)
            print_uart(frame[2:2+n])

        elif ftype == 0x02:
            code = frame[1]
            control_responses.append(frame)

    return got_any


def wait_response(code, timeout=2.0):
    deadline = time.monotonic() + timeout

    while time.monotonic() < deadline:
        pump_hid_once()

        for i, frame in enumerate(control_responses):
            if len(frame) >= 2 and frame[1] == code:
                return control_responses.pop(i)

        time.sleep(0.001)

    return None


def parse_status(frame):
    if frame is None or len(frame) < 30:
        return None

    return {
        "rx": u32le(frame, 2),
        "drop": u32le(frame, 6),
        "queued": u32le(frame, 10),
        "reports": u32le(frame, 14),
        "fail": u32le(frame, 18),
        "actual": u32le(frame, 22),
        "gp17": frame[26],
        "readable": frame[27],
        "last": frame[28],
        "mounted": frame[29],
    }


def fmt_status(s):
    return (
        f"rx={s['rx']} drop={s['drop']} queued={s['queued']} "
        f"reports={s['reports']} fail={s['fail']} "
        f"actual={s['actual']} GP17={s['gp17']} "
        f"readable={s['readable']} last={s['last']:02X} "
        f"mounted={s['mounted']}"
    )


def preflight():
    send_command("P")
    if wait_response(RESP_PONG, 1.5) is None:
        raise RuntimeError("v28 HID PONG failed")
    print("[preflight] HID PONG", flush=True)

    send_command("S")
    status_frame = wait_response(RESP_STATUS, 1.5)

    if status_frame is None:
        raise RuntimeError("v28 HID STATUS failed")

    status = parse_status(status_frame)
    print("[preflight] " + fmt_status(status), flush=True)

    send_command("C")
    if wait_response(RESP_CLEARED, 1.5) is None:
        raise RuntimeError("v28 HID CLEAR failed")
    print("[preflight] HID CLEARED", flush=True)


def run_rpiboot():
    waiting = threading.Event()
    stage0 = threading.Event()
    stage1 = threading.Event()

    proc = subprocess.Popen(
        [rpiboot, "-d", usbdir, "-v"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )

    def reader():
        assert proc.stdout is not None

        for line in proc.stdout:
            print("[usb] " + line, end="", flush=True)

            if "Waiting for BCM2835/6/7/2711/2712" in line:
                waiting.set()

            if "Found serial number 0" in line:
                stage0.set()

            if "Found serial number 1" in line:
                stage1.set()

    th = threading.Thread(target=reader, daemon=True)
    th.start()

    return proc, th, waiting, stage0, stage1


try:
    path = discover_hid()
    print("[hid] v28 bridge found", flush=True)

    dev = open_hid(path)
    time.sleep(0.15)

    preflight()

    print("[reset] HOLDING Pi RUN before rpiboot", flush=True)
    send_command("H")

    if wait_response(RESP_HELD, 1.5) is None:
        raise RuntimeError("v28 HID HELD acknowledgement failed")

    print("[reset] HID HELD", flush=True)
    time.sleep(0.50)

    proc, th, waiting, stage0, stage1 = run_rpiboot()

    if not waiting.wait(timeout=2.0):
        proc.terminate()
        raise RuntimeError("rpiboot never reached BCM wait state")

    print("[reset] rpiboot armed; RELEASING Pi RUN", flush=True)
    send_command("L")

    if wait_response(RESP_RELEASED, 1.5) is None:
        proc.terminate()
        raise RuntimeError("v28 HID RELEASED acknowledgement failed")

    print("[reset] HID RELEASED", flush=True)

    start = time.monotonic()
    next_notice = 5.0

    while proc.poll() is None:
        pump_hid_once()

        elapsed = time.monotonic() - start

        if not stage0.is_set() and elapsed >= next_notice:
            print(
                f"[usb] still waiting for fresh serial 0... {int(elapsed)}s",
                flush=True,
            )
            next_notice += 5.0

        if not stage0.is_set() and elapsed > 60.0:
            proc.terminate()
            raise RuntimeError(
                "fresh serial 0 not seen within 60s after reset release"
            )

        time.sleep(0.001)

    proc.wait(timeout=2.0)
    th.join(timeout=1.0)

    print(
        f"\n[usb] rpiboot exited: {proc.returncode} "
        f"stage0={int(stage0.is_set())} stage1={int(stage1.is_set())}",
        flush=True,
    )

    if not (
        proc.returncode == 0
        and stage0.is_set()
        and stage1.is_set()
    ):
        raise RuntimeError(
            "verified serial-0 -> serial-1 rpiboot cycle did not complete"
        )

    print("[uart] capturing for %.0fs" % capture_seconds, flush=True)

    deadline = time.monotonic() + capture_seconds

    while time.monotonic() < deadline:
        pump_hid_once()
        time.sleep(0.001)

    send_command("S")
    status_frame = wait_response(RESP_STATUS, 2.0)

    if status_frame is None:
        raise RuntimeError(
            "post-stream HID STATUS did not return"
        )

    status = parse_status(status_frame)

    print("\n[postflight] " + fmt_status(status), flush=True)
    print("[postflight] PASS: HID endpoint still responds", flush=True)
    print(
        f"[uart] host captured {len(captured)} byte(s)",
        flush=True,
    )

except Exception as exc:
    print(
        f"\n[runner] ERROR: {type(exc).__name__}: {exc}",
        flush=True,
    )
    traceback.print_exc()
    raise SystemExit(3)

finally:
    if dev is not None:
        try:
            dev.close()
        except Exception:
            pass

print("\n=== RUN COMPLETE ===", flush=True)
'@

$env:MD_RPIBOOT = $RpiBoot
$env:MD_USBDIR = $UsbDir
$env:MD_CAPTURE_SECONDS = [string]$CaptureSeconds

try {
    $py | python -

    if ($LASTEXITCODE -ne 0) {
        throw "runner failed with exit code $LASTEXITCODE"
    }
}
finally {
    Remove-Item Env:MD_RPIBOOT -ErrorAction SilentlyContinue
    Remove-Item Env:MD_USBDIR -ErrorAction SilentlyContinue
    Remove-Item Env:MD_CAPTURE_SECONDS -ErrorAction SilentlyContinue
}
