# UsbWarp

**High-performance USB device passthrough for WSL2 via Hyper-V HDV shared memory.**

UsbWarp replaces network-based USB/IP with a zero-copy MMIO ring protocol, targeting **<100μs round-trip latency** and **>1 GB/s throughput** for USB device access from within WSL2.

> ⚠️ **Early Development** — This is the first version where end-to-end data flow works. The code is under active development, has known limitations, and has not been hardened for production use. **Use at your own risk.** Kernel drivers can cause BSODs if something goes wrong.

## How It Works

```
┌─────────────────────────────────────────────────────────────┐
│  WSL2 (Linux)                                               │
│                                                             │
│  /dev/ttyACM0  ←→  cdc_acm  ←→  usbwarp.ko (Virtual HCD)  │
│                                       │                     │
│                              Guest-to-Host Ring (MMIO)      │
└───────────────────────────────────┼─────────────────────────┘
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
└── usbwarp_module/         Linux kernel module (out-of-tree)
    └── Virtual HCD, PCI probe, Ring producer/consumer, poll thread
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

# !!! Edit build.sh.example or create your own build script:
cp build.sh.example build.sh
chmod +x build.sh
./build.sh

# Output: usbwarp.ko
```

## Installation

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

# Reboot to activate (or replug USB devices)
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
# (Use hcsdiag.exe list  in PowerShell)

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

## Current Status

**What works:**

- ✅ Full USB enumeration (GET_DESCRIPTOR, SET_CONFIGURATION, SET_ADDRESS)
- ✅ Control transfers (standard, class, vendor — raw setup packet passthrough)
- ✅ Bulk IN/OUT transfers (zero-copy via shared memory)
- ✅ Interrupt IN transfers
- ✅ CDC ACM serial devices (tested with Raspberry Pi Pico / MicroPython)
- ✅ Device bind/unbind lifecycle
- ✅ Graceful shutdown with cancel propagation
- ✅ Circuit breaker, rate limiter, and orphan detection safety layers

**Known limitations:**

- No isochronous transfer support (audio/video devices won't work yet)
- Single VM session only
- Service runs in console mode (no Windows Service / SCM integration yet)
- Filter driver attaches to *all* USB devices (no per-device targeting yet)
- No automated driver signing (test signing required)
- Limited error recovery on device surprise-removal

## Roadmap

Planned features roughly in priority order:

- **Isochronous transfer support** — Enables USB audio interfaces, webcams, and other streaming devices
- **Batch URB submission** — Coalesce multiple URBs into a single Ring message for reduced overhead
- **ETW tracing** — Replace KdPrint with structured Event Tracing for Windows; sysfs stats on Linux side
- **CPU affinity for poll threads** — Pin host and guest poll threads to specific cores for consistent latency
- **SCM service mode** — Run UsbWarpService as a proper Windows service with auto-start
- **Configuration persistence** — Registry-based device binding that survives reboots
- **Multi-VM support** — Bind different USB devices to different WSL2 distributions simultaneously
- **Selective filter attachment** — Only attach UsbWarpFilter to devices that are actively bound, reducing overhead for unrelated USB devices
- **Hot-plug support** — Detect physical USB device insertion/removal and propagate to Guest automatically
- **USBDK-style driver replacement** — Optional mode to fully detach Windows function driver during bind for exclusive Guest access
- **Performance dashboard** — Real-time latency histograms and throughput metrics via a TUI or web interface
- **Signed driver packages** — Proper WHQL or attestation signing for easy deployment without test mode

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
```

## License

GPL-2.0-only. See [LICENSE.txt](LICENSE.txt).

## Acknowledgements

Architecture informed by studying [usbipd-win](https://github.com/dorssel/usbipd-win) and [usbip-win2](https://github.com/vadimgrn/usbip-win2). UsbWarp takes a fundamentally different approach (shared memory MMIO vs. network sockets) but learned from their solutions to Windows USB stack constraints, particularly the need for a filter driver to hold a legitimate USBD_HANDLE.
