# UsbWarp

**High-performance USB device passthrough for WSL2 via Hyper-V HDV shared memory.**

UsbWarp replaces network-based USB/IP with a zero-copy MMIO ring protocol, targeting **<100μs round-trip latency** and **>1 GB/s throughput** for USB device access from within WSL2.

> **⚠️ Alpha Release (v0.1.0-alpha)**
>
> This is the first working release. End-to-end USB data transfer has been
> verified with a Raspberry Pi Pico (CDC ACM serial) through 22 hours of
> continuous operation and 374-vector protocol fuzz testing with zero BSODs.
>
> **This is kernel-level software. Untested device types may cause BSODs.
> Use in a VM with snapshots. Not for production.**

## How It Works

```
┌──────────────────────────────────────────────────────────────────────────┐
│  WSL2 (Linux)                                                            │
│                                                                          │
│  /dev/ttyACM0 (or other usermode interface)  ←→ usbwarp.ko (Virtual HCD) │
│                                       │                                  │
│                              Guest-to-Host Ring (MMIO)                   │
└───────────────────────────────────┼──────────────────────────────────────┘
                          Section-backed MMIO
                        (Hyper-V HDV, zero-copy)
┌───────────────────────────────────┼─────────────────────────┐
│  Windows Host                     │                         │
│                                   │                         │
│  UsbWarp.sys ←── shared memory ───┘                         │
│      │                                                      │
│      │── custom IOCTL ──→ UsbWarpFilter.sys (USB FiDO)      │
│      │                         │                            │
│  UsbWarpService.exe            │  IOCTL_INTERNAL_USB_SUBMIT │
│      │                         ↓                            │
│  UsbWarpController.exe    Physical USB Device               │
└─────────────────────────────────────────────────────────────┘
```

**Key design choices:**

- **Zero-copy shared memory** — 32 MB Section-backed MMIO mapped into both Guest and Host kernel space. USB data buffers live in shared memory; no `memcpy` on the data path.
- **SPSC lock-free ring protocol** — Single-producer/single-consumer rings with acquire/release semantics. No locks on the hot path.
- **Kernel-to-kernel** — URB proxy runs entirely in kernel mode (UsbWarp.sys ↔ UsbWarpFilter.sys ↔ USB physical stack). No user-mode round trips for data transfer.
- **HDV-based PCI device** — The shared memory region appears as a PCI BAR in the Guest via Hyper-V Device Virtualization, enabling standard MMIO access from the Linux kernel module.

## Project Structure

```
USBWarp.sln                 Visual Studio 2022 solution
│
├── include/                Shared protocol headers (all components)
│   ├── usbwarp_shared.h        Ring protocol, message types, control block
│   ├── usbwarp_ioctl.h         Driver ↔ Service IOCTL interface
│   ├── usbwarp_pipe.h          Service ↔ Controller named pipe protocol
│   └── usbwarp_filter_ioctl.h  UsbWarp.sys ↔ UsbWarpFilter.sys interface
│
├── USBWarp/                Windows kernel driver (WDM/KMDF)
│   └── Ring consumer, SHM mapping, URB proxy via filter, safety layers
│
├── UsbWarpFilter/          USB class upper filter driver (WDM)
│   └── USBD_CreateHandle, URB submission proxy, cancel propagation
│
├── USBWarpService/         User-mode service (console, C++)
│   └── HDV session, SHM creation, named pipe server, driver client
│
├── USBWarpController/      CLI tool (C)
│   └── list/bind/unbind/status commands
│
├── usbwarp_module/         Linux kernel module (out-of-tree)
│   └── Virtual HCD, PCI probe, Ring producer/consumer, poll thread
│
└── tests/                  Test suite
    ├── ring_test.c             Layer 1: Ring protocol unit tests (54 tests)
    ├── ring_fuzz.c             Layer 2: Protocol fuzz generator (374 vectors)
    ├── run_fuzz.sh             Layer 2: Fuzz runner with pause/resume
    ├── layer3_test.sh          Layer 3: Stress, bind/unbind, endurance
    └── pico_helper.py          Persistent serial connection helper
```

## Building

### Prerequisites

- **Windows:** Visual Studio 2022 with C++ Desktop workload + [Windows Driver Kit (WDK) 10.0.26100](https://learn.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk)
- **Linux:** Kernel headers for your WSL2 kernel (see [WSL2-Linux-Kernel](https://github.com/microsoft/WSL2-Linux-Kernel))

### Windows Components

Open `USBWarp.sln` in Visual Studio 2022 and build:

- **USBWarp** and **UsbWarpFilter** — `Debug | x64` (for WinDbg-friendly symbols)
- **USBWarpService** and **USBWarpController** — `Release | x64`

All four projects should build without errors. Output binaries are in `x64/Debug/` and `x64/Release/`.

### Linux Module

```bash
cd usbwarp_module

# Edit build.sh.example or create your own build script:
cp build.sh.example build.sh
chmod +x build.sh
./build.sh

# Output: usbwarp.ko
```

## Installation

### Step 0: Enable Test Signing

The drivers are not WHQL-signed. Windows requires test signing mode to load them:

```powershell
bcdedit /set testsigning on
# Reboot required
shutdown /r /t 0
```

After reboot, a "Test Mode" watermark appears on the desktop. This is normal.

### Step 1: Install UsbWarpFilter.sys

The filter driver must be installed as a USB class upper filter. This requires a one-time setup:

```powershell
# Copy driver to system directory
copy x64\Debug\UsbWarpFilter.sys C:\Windows\System32\drivers\

# Register as USB class upper filter
reg add "HKLM\SYSTEM\CurrentControlSet\Control\Class\{36FC9E60-C465-11CF-8056-444553540000}" ^
    /v UpperFilters /t REG_MULTI_SZ /d "UsbWarpFilter" /f

# Create service entry
sc create UsbWarpFilter type=kernel binPath=System32\drivers\UsbWarpFilter.sys

# Reboot to activate
shutdown /r /t 0
```

### Step 2: Install UsbWarp.sys

```powershell
copy x64\Debug\UsbWarp.sys C:\Windows\System32\drivers\
sc create UsbWarp type=kernel binPath=System32\drivers\UsbWarp.sys
sc start UsbWarp
```

### Step 3: Load Linux Module

```bash
# Load USB core dependencies first
sudo modprobe usbcore
sudo modprobe cdc_acm    # or whichever class driver your device needs

# Load UsbWarp
sudo insmod usbwarp.ko
```

### Step 4: Start Service and Bind a Device

```powershell
# Find your WSL2 VM GUID
# (Use hcsdiag.exe list)

# Start the service (console mode for now)
.\USBWarpService.exe --id <WSL2-VM-GUID>

# In another terminal, list USB devices
.\USBWarpController.exe list

# Bind a device by its BUSID
.\USBWarpController.exe bind <BUSID>
```

In WSL2, the device should appear:

```bash
dmesg | tail -5
# usb 1-1: new full-speed USB device number 2 using usbwarp
# cdc_acm 1-1:1.0: ttyACM0: USB ACM device

ls /dev/ttyACM0
```

## Test Results

All testing performed with Raspberry Pi Pico (RP2040) running MicroPython, connected as USB CDC ACM serial device.

| Layer | Test | Result | Details |
|-------|------|--------|---------|
| **1** | Ring protocol unit tests | **54/54 pass** | Alignment, wrap-around, fill/drain, 10K message data integrity |
| **2** | Protocol fuzz (no device) | **374 vectors, zero BSOD** | Bad magic, invalid lengths, type/version/device_id/endpoint fuzzing, garbage injection |
| **2** | Protocol fuzz (with device) | **374 vectors, zero BSOD** | SET_ADDRESS blocked, wLength mismatch blocked, re-negotiate rejected |
| **3A** | Data echo verification | **100/100 correct** | Random arithmetic via MicroPython REPL |
| **3B** | Bind/unbind cycles | **50/50 pass** | ~14s per cycle, zero resource leaks |
| **3C** | Sustained load (1 hour) | **2,642 iter, zero errors** | Slab Δ=560kB (normal) |
| **3D** | Abnormal recovery | **4/5 pass** | Double bind rejected, rapid cycles recovered, mid-transfer unbind recovered |
| **4** | 22-hour endurance | **58,708 iter, zero errors** | Orphan mode recovery verified on sleep/wake |

### Safety layers validated by fuzz testing

- **L1** — Message magic validation
- **L2** — Message type and protocol version checks
- **L3** — Device ID bounds and binding verification
- **L4** — Setup packet validation (SET_ADDRESS block, wLength/direction consistency)
- **L5** — Buffer offset bounds checking
- **Ring recovery** — Automatic scan-forward on corruption, full reset on unrecoverable damage
- **Re-negotiate rejection** — Session locked after initial handshake
- **Duplicate bind detection** — Container ID comparison prevents double-binding same physical device

## Current Status

**What works:**

- ✅ Full USB enumeration (GET_DESCRIPTOR, SET_CONFIGURATION, SET_ADDRESS)
- ✅ Control transfers (standard, class, vendor — raw setup packet passthrough)
- ✅ Bulk IN/OUT transfers (zero-copy via shared memory)
- ✅ Interrupt IN transfers
- ✅ CDC ACM serial devices (tested with Raspberry Pi Pico / MicroPython)
- ✅ Device bind/unbind lifecycle with duplicate detection
- ✅ Graceful shutdown with URB cancel propagation through filter driver
- ✅ Ring corruption recovery (scan-forward + reset)
- ✅ Circuit breaker, rate limiter, and orphan detection safety layers
- ✅ SHM page locking for safe DPC-level access

**Known limitations:**

- Only tested with CDC ACM devices (Pi Pico) — other device types may have issues
- No isochronous transfer support (audio/video devices won't work)
- No hot-plug detection — unbind before unplugging; if you forget, unbind + rebind recovers
- Single VM session only
- Service runs in console mode (no Windows Service / SCM integration)
- Filter driver attaches to all USB class devices (no per-device targeting)
- Drivers are not signed (test signing required)
- Full-speed devices only validated; high-speed/super-speed devices untested

## Roadmap

Planned features roughly in priority order:

- **Hot-plug support** — Detect physical USB device insertion/removal and propagate to Guest automatically via `IRP_MN_SURPRISE_REMOVAL` handling in the filter driver
- **High-speed / super-speed device testing** — Validate with USB 2.0/3.0 bulk devices (e.g., RTL-SDR, USB storage)
- **Isochronous transfer support** — Enables USB audio interfaces, webcams, and other streaming devices
- **SCM service mode** — Run UsbWarpService as a proper Windows service with auto-start
- **Selective filter attachment** — Only attach UsbWarpFilter to devices that are actively bound, reducing overhead for unrelated USB devices
- **ETW tracing** — Replace KdPrint with structured Event Tracing for Windows; sysfs stats on Linux side
- **Batch URB submission** — Coalesce multiple URBs into a single Ring message for reduced overhead
- **Configuration persistence** — Registry-based device binding that survives reboots
- **Multi-VM support** — Bind different USB devices to different WSL2 distributions simultaneously
- **Performance dashboard** — Real-time latency histograms and throughput metrics
- **Signed driver packages** — WHQL or attestation signing for deployment without test mode

## Running Tests

```bash
# Layer 1: Ring protocol (no hardware needed)
cd tests
gcc -O2 -Wall -I../include -o ring_test ring_test.c
./ring_test

# Layer 2: Protocol fuzz (requires usbwarp.ko loaded with debug=1)
sudo insmod usbwarp.ko debug=1
gcc -O2 -Wall -I../include -o ring_fuzz ring_fuzz.c
sudo ./run_fuzz.sh -v

# Layer 3: Stress tests (requires device bound)
sudo ./layer3_test.sh -t 3a                          # data echo
sudo ./layer3_test.sh -t 3b -b BUSID -s CTRL_PATH    # bind/unbind cycles
sudo ./layer3_test.sh -t 3c -c 3600                   # 1-hour sustained
sudo ./layer3_test.sh -t 3d -b BUSID -s CTRL_PATH    # abnormal recovery

# Layer 4: 24-hour endurance
sudo ./layer3_test.sh -t 3c -c 86400
```

## Uninstalling

```powershell
# Stop and remove drivers
sc stop UsbWarp
sc delete UsbWarp

# Remove filter registration
reg delete "HKLM\SYSTEM\CurrentControlSet\Control\Class\{36FC9E60-C465-11CF-8056-444553540000}" ^
    /v UpperFilters /f
sc stop UsbWarpFilter
sc delete UsbWarpFilter

# Reboot
shutdown /r /t 0

# Clean up files
del C:\Windows\System32\drivers\UsbWarp.sys
del C:\Windows\System32\drivers\UsbWarpFilter.sys

# Disable test signing (optional)
bcdedit /set testsigning off
```

## License

GPL-2.0-only. See [LICENSE](LICENSE).

## Acknowledgements

Architecture informed by studying [usbipd-win](https://github.com/dorssel/usbipd-win) and [usbip-win2](https://github.com/vadimgrn/usbip-win2). UsbWarp takes a fundamentally different approach (shared memory MMIO vs. network sockets) but learned from their solutions to Windows USB stack constraints, particularly the need for a filter driver to hold a legitimate USBD_HANDLE.
