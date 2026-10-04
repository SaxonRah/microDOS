param(
    [string]$Repo = "C:\microDOS",
    [string]$KernelImage = "",
    [int]$CaptureSeconds = 60,
    [switch]$NoBuild,
    [switch]$Interactive
)

$ErrorActionPreference = "Stop"

$Repo = (Resolve-Path $Repo).Path
$BuildScript = Join-Path $Repo "md_pi0w_baremetal.ps1"
$DefaultKernel = Join-Path $Repo "build-pi0w\out\kernel8.img"
$BuiltKernel = $DefaultKernel
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

if ($KernelImage -ne "") {
    if ([System.IO.Path]::IsPathRooted($KernelImage)) {
        $BuiltKernel = [System.IO.Path]::GetFullPath($KernelImage)
    } else {
        $BuiltKernel = [System.IO.Path]::GetFullPath((Join-Path $Repo $KernelImage))
    }
}

if (-not (Test-Path $BuiltKernel)) {
    throw "Built kernel not found: $BuiltKernel"
}

Write-Host ""
Write-Host "=== STAGE ==="
Copy-Item $BuiltKernel $UsbKernel -Force
$kernel = Get-Item $UsbKernel
Write-Host ("source: {0}" -f $BuiltKernel)
Write-Host ("kernel8.img: {0} bytes  {1}" -f $kernel.Length, $kernel.LastWriteTime)

Get-Process rpiboot -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 250

Write-Host ""
Write-Host "=== V29 HID BIDIRECTIONAL UART BRIDGE ==="
Write-Host "No CDC/COM port is used. Transport is 64-byte USB HID interrupt reports."
if ($Interactive) {
    Write-Host "Interactive keyboard input is ENABLED after rpiboot completes."
}

$py = @'
import os
import struct
import subprocess
import threading
import time
import traceback

try:
    import hid
except Exception as exc:
    print(
        "[runner] Python package 'hidapi' is required for v29.",
        flush=True,
    )
    print(
        "[runner] Install once with: python -m pip install hidapi",
        flush=True,
    )
    print(f"[runner] import error: {exc}", flush=True)
    raise SystemExit(4)

interactive = os.environ.get("MD_INTERACTIVE", "0") == "1"

if interactive:
    try:
        import msvcrt
    except Exception as exc:
        print(f"[runner] msvcrt unavailable: {exc}", flush=True)
        raise SystemExit(5)

VID = 0xCAFE
PID = 0x4028

RESP_PONG = 1
RESP_CLEARED = 2
RESP_HELD = 3
RESP_RELEASED = 4
RESP_STATUS = 5

HOST_FRAME_UART_TX = 0x10
HOST_UART_PAYLOAD = 62

rpiboot = os.environ["MD_RPIBOOT"]
usbdir = os.environ["MD_USBDIR"]
capture_seconds = float(os.environ["MD_CAPTURE_SECONDS"])

dev = None
captured = bytearray()
control_responses = []

# Keep Pi UART output off-screen until rpiboot/setup has completely finished.
# This prevents runner [xxx] status lines from being injected into the DOS
# prompt after COMMAND.COM has already started.
uart_live = False
uart_deferred = bytearray()


def u32le(b, off):
    return struct.unpack_from("<I", b, off)[0]


def discover_hid(timeout=12.0):
    deadline = time.monotonic() + timeout

    while time.monotonic() < deadline:
        found = hid.enumerate(VID, PID)

        for item in found:
            product = item.get("product_string") or ""

            if "microDOS HID UART bridge v29" in product:
                return item["path"]

        time.sleep(0.25)

    raise RuntimeError("v29 HID bridge not found after 12 seconds")


def open_hid(path):
    d = hid.device()
    d.open_path(path)
    d.set_nonblocking(1)
    return d


def hid_write_payload(payload64):
    if len(payload64) != 64:
        raise ValueError("device HID payload must be exactly 64 bytes")

    # hidapi write buffer byte 0 is Report ID.
    # This device uses report ID 0.
    packet = bytes([0]) + bytes(payload64)
    written = dev.write(packet)

    if written <= 0:
        raise RuntimeError("HID OUT report write failed")


def send_command(ch):
    payload = bytearray(64)
    payload[0] = ord(ch)
    hid_write_payload(payload)


def send_uart_bytes(data):
    if not data:
        return

    pos = 0

    while pos < len(data):
        chunk = data[pos:pos + HOST_UART_PAYLOAD]
        payload = bytearray(64)
        payload[0] = HOST_FRAME_UART_TX
        payload[1] = len(chunk)
        payload[2:2 + len(chunk)] = chunk
        hid_write_payload(payload)
        pos += len(chunk)


def print_uart(data):
    if not data:
        return

    captured.extend(data)

    if uart_live:
        print(data.decode(errors="replace"), end="", flush=True)
    else:
        uart_deferred.extend(data)


def enter_dos_console():
    global uart_live

    print("", flush=True)
    print("=== DOS CONSOLE ===", flush=True)

    uart_live = True

    if uart_deferred:
        print(
            bytes(uart_deferred).decode(errors="replace"),
            end="",
            flush=True,
        )
        uart_deferred.clear()


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
    if frame is None or len(frame) < 43:
        return None

    return {
        "rx": u32le(frame, 2),
        "rx_drop": u32le(frame, 6),
        "rx_queued": u32le(frame, 10),
        "reports": u32le(frame, 14),
        "fail": u32le(frame, 18),
        "actual": u32le(frame, 22),
        "gp17": frame[26],
        "readable": frame[27],
        "last_rx": frame[28],
        "mounted": frame[29],
        "tx": u32le(frame, 30),
        "tx_drop": u32le(frame, 34),
        "tx_queued": u32le(frame, 38),
        "last_tx": frame[42],
    }


def fmt_status(s):
    return (
        f"rx={s['rx']} rx_drop={s['rx_drop']} "
        f"rx_queued={s['rx_queued']} reports={s['reports']} "
        f"fail={s['fail']} actual={s['actual']} "
        f"GP17={s['gp17']} readable={s['readable']} "
        f"last_rx={s['last_rx']:02X} mounted={s['mounted']} "
        f"tx={s['tx']} tx_drop={s['tx_drop']} "
        f"tx_queued={s['tx_queued']} last_tx={s['last_tx']:02X}"
    )


def preflight():
    send_command("P")
    if wait_response(RESP_PONG, 1.5) is None:
        raise RuntimeError("v29 HID PONG failed")
    print("[preflight] HID PONG", flush=True)

    send_command("S")
    status_frame = wait_response(RESP_STATUS, 1.5)

    if status_frame is None:
        raise RuntimeError("v29 HID STATUS failed")

    status = parse_status(status_frame)
    if status is None:
        raise RuntimeError("v29 HID STATUS payload too short")

    print("[preflight] " + fmt_status(status), flush=True)

    send_command("C")
    if wait_response(RESP_CLEARED, 1.5) is None:
        raise RuntimeError("v29 HID CLEAR failed")
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


def poll_keyboard():
    if not interactive:
        return

    while msvcrt.kbhit():
        ch = msvcrt.getwch()

        # Windows extended-key prefix.  Consume and ignore the scan code.
        if ch in ("\x00", "\xe0"):
            if msvcrt.kbhit():
                _ = msvcrt.getwch()
            continue

        # DOS console input wants CR for Enter.
        if ch == "\n":
            ch = "\r"

        code = ord(ch)

        if code <= 0xFF:
            send_uart_bytes(bytes([code]))


try:
    path = discover_hid()
    print("[hid] v29 bridge found", flush=True)

    dev = open_hid(path)
    time.sleep(0.15)

    preflight()

    print("[reset] HOLDING Pi RUN before rpiboot", flush=True)
    send_command("H")

    if wait_response(RESP_HELD, 1.5) is None:
        raise RuntimeError("v29 HID HELD acknowledgement failed")

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
        raise RuntimeError("v29 HID RELEASED acknowledgement failed")

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

    print("[uart] capture window: %.0fs" % capture_seconds, flush=True)

    if interactive:
        print(
            "[kbd] keyboard ready: Enter sends CR; "
            "Ctrl+] requests microDOS statistics.",
            flush=True,
        )

    # All host-side setup/status is now complete.  From this point onward,
    # stdout is the Pi/DOS terminal.  UART bytes accumulated while rpiboot was
    # finishing are emitted in-order here, so DOS never gets split by late
    # [usb]/[uart]/[kbd] runner messages.
    enter_dos_console()

    deadline = time.monotonic() + capture_seconds

    while time.monotonic() < deadline:
        pump_hid_once()
        poll_keyboard()
        time.sleep(0.001)

    send_command("S")
    status_frame = wait_response(RESP_STATUS, 2.0)

    if status_frame is None:
        raise RuntimeError("post-stream HID STATUS did not return")

    status = parse_status(status_frame)

    if status is None:
        raise RuntimeError("post-stream v29 STATUS payload too short")

    print("\n[postflight] " + fmt_status(status), flush=True)
    print("[postflight] PASS: HID endpoint still responds", flush=True)
    print(
        f"[uart] host captured {len(captured)} byte(s)",
        flush=True,
    )

except KeyboardInterrupt:
    print("\n[runner] keyboard interrupt: ending capture cleanly", flush=True)

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
$env:MD_INTERACTIVE = if ($Interactive) { "1" } else { "0" }

$TempPy = Join-Path $env:TEMP ("microdos-pi0w-v29-{0}.py" -f $PID)

try {
    # Do NOT pipe the Python source through stdin here. Interactive mode needs
    # the child Python process attached to the real Windows console so
    # msvcrt.kbhit()/getwch() can see keyboard input.
    [System.IO.File]::WriteAllText(
        $TempPy,
        $py,
        [System.Text.UTF8Encoding]::new($false)
    )

    & python $TempPy

    if ($LASTEXITCODE -ne 0) {
        throw "runner failed with exit code $LASTEXITCODE"
    }
}
finally {
    Remove-Item $TempPy -Force -ErrorAction SilentlyContinue
    Remove-Item Env:MD_RPIBOOT -ErrorAction SilentlyContinue
    Remove-Item Env:MD_USBDIR -ErrorAction SilentlyContinue
    Remove-Item Env:MD_CAPTURE_SECONDS -ErrorAction SilentlyContinue
    Remove-Item Env:MD_INTERACTIVE -ErrorAction SilentlyContinue
}
