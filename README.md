# UsbWarp

**High-performance USB device passthrough for WSL2 via Hyper-V HDV shared memory.**

UsbWarp replaces network-based USB/IP with a shared-memory MMIO ring protocol. Windows and Linux kernels exchange USB transfers directly; the Windows service manages capture, configuration and session lifetime.

> **Alpha release: v0.2.1-alpha**
>
> This release adds persistent VM selection and automatic discovery of WSL's
> current HCS ID, building on the USB 3.x, ISO and capture support in v0.2.0-alpha.
> The current hardware validation uses one USB 3 mass-storage device through a
> hub in Windows 11 / nested WSL2. ISO peripherals, UAS and all SSP lane modes
> still need hardware testing. See [release notes](docs/release-0.2.1-alpha.txt).
>
> Drivers are test-signed, not WHQL-certified. This is a prerelease for test
> environments; use a VM with a snapshot.

The `vm` commands below require the v0.2.1-alpha Service and Controller.
Upgrades retain an existing fixed-ID selection until `vm use` changes it.

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
                        (Hyper-V HDV shared RAM)
┌───────────────────────────────────┼─────────────────────────┐
│  Windows Host                     │                         │
│                                   │                         │
│  UsbWarp.sys ←── shared memory ───┘                         │
│      │                                                      │
│      │── custom IOCTL ──→ UsbWarpFilter.sys (capture FDO)   │
│      │                         │                            │
│  UsbWarpService.exe            │  IOCTL_INTERNAL_USB_SUBMIT │
│      │                         ↓                            │
│  UsbWarpController.exe    Physical USB Device               │
└─────────────────────────────────────────────────────────────┘
```

**Key design choices:**

- **Shared-memory data path** — 32 MB Section-backed MMIO mapped into Guest and Host kernel space. The Windows USB stack accesses shared buffers directly. Linux copies between URB buffers and MMIO; ISO packets are packed on submission and scattered back to their original offsets on completion. This is not end-to-end zero-copy.
- **SPSC ring protocol, lock-free consumer** — Single-consumer rings with acquire/release index semantics; concurrent producers use a leaf spinlock. An explicit coherent-RAM flag permits write-back mapping of the section; older hosts retain the uncached mapping. A separate uncached BAR carries G2H doorbell writes.
- **Kernel-to-kernel** — URB proxy runs entirely in kernel mode (UsbWarp.sys ↔ UsbWarpFilter.sys ↔ USB physical stack). No user-mode round trips for data transfer.
- **One binding authority** — UsbWarp.sys owns bindings, negotiation, transport state and device counters. SCM/HDV lifecycle and target capture are separate service components. The capture journal records PnP changes to undo; device listings always come from the kernel.
- **Exclusive target capture** — UsbWarpFilter is a KMDF function driver installed only on the selected physical device instance. It replaces the Windows function driver for the duration of capture, holds one kernel file lease, and retains the kernel data path for control, bulk, interrupt, ISO and USB 3.x streams. Other USB devices have no UsbWarpFilter attachment.
- **Kernel event wakeup** — When both endpoints support it, HDV doorbell writes signal the kernel poller's event directly. USB completions also wake it directly, avoiding timer-tick delays between storage command phases. Capability negotiation retains polling compatibility.
- **Optional native MSI-X notification** — Adaptive polling is the default. With `h2g_irq=1` on verified x64 Hyper-V builds, Windows kernel completions notify Linux directly through one PCI MSI-X vector. The service handles configuration only. Temporary GuestOsId discovery, restore watchdogs, pending/mask handling and polling fallback are described in [the backend notes](docs/kernel-interrupts.txt), including the native ABI's public-documentation and certification limits.
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
├── UsbWarpFilter/          Dedicated USB capture function driver (KMDF)
│   └── Exclusive lease, USBD/URB proxy, PnP/power/queue cancellation
│
├── USBWarpService/         SCM Windows service + console host (C++)
│   └── Session supervisor, HDV/SHM lifetime, local IPC, asynchronous driver client
│
├── USBWarpController/      CLI tool (C)
│   └── list/bind/unbind/status commands
│
├── usbwarp_module/         Linux kernel module (out-of-tree)
│   └── USB2 + USB3.2 HCDs, PCI probe, streams, SG/ZLP, Ring, poll thread
│
└── tests/                  Test suite
    ├── ring_test.c             Layer 1: Ring protocol unit tests (54 tests)
    ├── build_service_tests.ps1 Service lifecycle, cancellation, IPC and SCM tests
    ├── fuzz/ring_fuzz.c             Layer 2: Protocol fuzz generator (374 vectors)
    ├── fuzz/run_fuzz.sh             Layer 2: Fuzz runner with pause/resume
    ├── layer3_test.sh          Layer 3: Stress, bind/unbind, endurance
    └── pico_helper.py          Persistent serial connection helper
```

## Building

### Prerequisites

- **Windows:** Visual Studio 2022 with C++ Desktop workload + [Windows Driver Kit (WDK) 10.0.26100](https://learn.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk)
- **Linux:** Kernel headers for your WSL2 kernel (see [WSL2-Linux-Kernel](https://github.com/microsoft/WSL2-Linux-Kernel))

### Windows Components

Open `USBWarp.sln` in Visual Studio 2022 and build `Release | x64`, or use an
x64 Developer PowerShell:

```powershell
msbuild USBWarp.sln /m /p:Configuration=Release /p:Platform=x64
```

Building the solution places binaries and PDBs under `x64/Release/`.
The capture driver package is in `x64/Release/UsbWarpFilter/`. Keep the matching
PDB files for debugging. `Debug | x64` is also available for driver development.

### Linux Module

```bash
cd usbwarp_module

# Point this at the prepared source/build tree for the running WSL kernel:
sudo ln -sf /home/oh2/WSL2-Linux-Kernel /lib/modules/$(uname -r)/build
KBUILD_MODPOST_WARN=1 make

# Output: usbwarp.ko
```

Use the running kernel's exact version and configuration. `KBUILD_MODPOST_WARN=1`
permits a build tree without `Module.symvers`; it does not make mismatched modules
compatible. The optional prebuilt module is only for the kernel named in its
archive. See [release build instructions](docs/releasing.txt) for packaging.

## Installation

### Step 0: Enable Test Signing

The drivers are not WHQL-signed. The release ZIP includes a public test
certificate; import it only on the test machine as described in the ZIP's
`INSTALL.txt`. Windows also requires test signing mode to load these drivers:

```powershell
bcdedit /set testsigning on
# Reboot required
shutdown /r /t 0
```

After reboot, a "Test Mode" watermark appears on the desktop. This is normal.

### Step 1: Install UsbWarpFilter.sys

Build and sign the generated INF/SYS/CAT package. Put it in the `capture`
subdirectory of the installed service and stage it in the driver store:

```powershell
.\scripts\install_capture.ps1 -ServiceDirectory C:\USBWarp `
    -PackageDirectory .\x64\Release\UsbWarpFilter
```

The INF uses a synthetic ID that never matches a real USB device automatically.
The service selects it using `DiInstallDevice` on the exact device instance;
it never updates every device sharing a VID/PID. Do not register this driver
in USB class `UpperFilters`. When upgrading the old class-filter version, stop
the old service and let it restore every capture first. The script preserves
other class filters and requests a reboot only to unload the old attachment.
An outstanding restoration journal prevents upgrading or removing the service.

### Step 2: Install UsbWarp.sys

```powershell
Copy-Item x64\Release\USBWarp.sys C:\Windows\System32\drivers\
sc.exe create UsbWarp type= kernel binPath= System32\drivers\UsbWarp.sys
sc.exe start UsbWarp
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

The executable supports both a console host and an SCM-managed Windows service.
Install the kernel driver first; the installed service depends on `UsbWarp`.

```powershell
# Console mode (requires an elevated shell — the manifest asks for admin):
.\USBWarpService.exe --wsl

# --- or install as an auto-start Windows service (admin) ---
.\USBWarpService.exe --install
# Configure the WSL owner's service Log On account as described below.
sc.exe start UsbWarpService
# Select a persistent target separately from service installation:
.\USBWarpController.exe vm list
.\USBWarpController.exe vm use --wsl
.\USBWarpController.exe vm status
# Stop or restart through SCM:
sc.exe stop UsbWarpService
sc.exe start UsbWarpService
# config persists as one checked Config value under HKLM\SOFTWARE\UsbWarp\Service;
# logs go to %ProgramData%\UsbWarp\service.log
# remove with: USBWarpService.exe --uninstall
```

Installation uses a quoted executable path, automatic startup, and bounded SCM
recovery actions (restart after 5, 15 and 60 seconds, then stop retrying until the
failure count resets after one day). Re-running `--install` updates configuration;
restart the service to apply it. It runs as LocalSystem by default. Logs are in
`%ProgramData%\UsbWarp\service.log`, with the Application event log as a fallback.

For user-owned WSL VMs, configure the service's **Log On** account in
`services.msc` to the WSL owner with administrator/Hyper-V access and the
"Log on as a service" right. In the tested Windows 11 / WSL 2.6.3 environment,
LocalSystem could open the compute-system handle but HDV initialization returned
access denied; running the SCM service as the WSL owner succeeded. Re-running
`--install` preserves an existing service account.

SCM `RUNNING` means that the host and its local control interface are ready.
`USBWarpController.exe status` reports the separate USB session stage, including
`WAITING_FOR_TARGET`, `WAITING_FOR_DRIVER`, `WAITING_FOR_VM` and `WAITING_FOR_GUEST`. The service can be
stopped while waiting. Driver/VM disconnects close the old session and retry the
configured VM without exiting the process. Bind requests fail until the kernel
has completed guest negotiation; `--debug` only changes diagnostics.

Installation does not require a VM ID. `vm use --wsl` persists a WSL selector;
the service uses the public `HcsEnumerateComputeSystems` API to resolve the
current ID on connection and reconnection. Starting WSL normally is sufficient;
the service does not launch a distribution or boot a VM. Reconnected sessions
begin with no USB bindings; bind the intended devices explicitly.

For other HCS-visible virtual machines use `vm use --name "VM name"`, optionally
with `--owner "VMMS"` (or the Owner displayed by `vm list`). Names and owners
match exactly, ignoring case; wildcards are not interpreted. An explicit
`vm use --id GUID` retains fixed-ID behavior. `vm clear` releases the empty
session and leaves the service running without a selected target. Selection
changes require elevation, persist atomically and do not require reinstalling
or restarting the service. Existing bindings or pending restoration records
prevent switching; unbind and finish recovery first.

WSL often has no HCS Name: `--wsl` means the unique running virtual machine with
HCS Owner `WSL` visible to the service. This is not a distribution-name or user-SID
filter. Multiple matches are reported as ambiguous and never resolved by taking
the first entry. Configure the service's appropriate account, and use an explicit
ID when a shared multi-user host cannot identify one WSL target unambiguously.
HCS visibility does not guarantee HDV support or access: `vm status` distinguishes
the saved selector, resolved ID, attached ID and errors. See [VM selection](docs/vm-selection.txt).

IPC accepts up to eight concurrent local clients. Authenticated users can query;
bind/unbind and target changes require an enabled Administrator or LocalSystem token. Remote clients,
oversized/partial messages and invalid paths are rejected. Service/session start
and stop are managed through SCM (or Ctrl+C in console mode); the old reserved
pipe command numbers for those actions are not implemented.

`stats` displays the counters supplied by the kernel: processed URBs, combined
transferred bytes and G2H ring occupancy. Directional totals, latency percentiles
and the other reserved protocol statistics are not collected yet.

On stop, the service signals cancellation and waits for actual I/O and worker
completion, drains the kernel session, unregisters the doorbell, unmaps the BAR
and tears down its HDV host and section. A crashed service triggers driver cleanup through its file
object. Slow USB/HCS operations can extend `STOP_PENDING`; checkpoints continue
while cleanup waits. In particular, [HcsCancelOperation is not implemented by Windows](https://learn.microsoft.com/en-us/virtualization/api/hcs/reference/hcscanceloperation),
so pending native operations retain their resources until they finish.

[FlexibleIov supports Add only](https://learn.microsoft.com/en-us/virtualization/api/hcs/reference/hcsmodifycomputesystem).
Each attachment therefore uses a fresh instance ID. Windows reclaims inactive
PCI shells at VM teardown; Linux retires their USB buses and mappings immediately
on orderly shutdown, or after loss detection on a crash. Both BARs are 64-bit,
which avoids exhausting WSL's small low-MMIO window on repeated service restarts.

Close applications using the target before binding. For a mounted filesystem,
flush/dismount it before changing ownership. The capture mechanism itself has
no disk, HID or other function-class special cases: PnP replaces the whole
physical device's function driver, including all functions of a composite
device. Hubs/controllers and individual composite interfaces are rejected.

Before changing PnP state, the service flushes an undo journal under
`HKLM\SOFTWARE\UsbWarp\Service\DeviceCapture` containing the original binding
kind (installed driver or no driver), INF, installation section, exact driver
model and per-device filters. It resets
the target port and waits for that exact instance to be removed and restarted
before binding the new capture interface. Only the core driver's kernel file
can claim and use its endpoints. Closing that lease drains transfers and retires
the capture incarnation; it cannot be reopened as a new lease.

On release, the core drains I/O before the service resets the device, reinstalls
the original driver/model and filters, and rebuilds the native stack. Startup
recovers abandoned captures; unplugged devices retain their undo records until
they return. A PnP veto, missing package or incomplete reset is reported as a
failure with recovery state retained. No native disk child is disabled.

Devices without a Windows driver are supported. Enumeration and targeting use
the USB devnode and the parent hub's bus-reported port, without requiring an
enabled function interface or driver registry key. Absence is checked against
both the installed model and the INF/service properties; Code 1/28 is accepted
for a missing function driver, while real start/disable/hardware failures are
still rejected. On return, a device originally without a driver is reset while
the capture driver still exists, then explicitly assigned the NULL driver.
Windows is not asked to choose a replacement driver, and restoration does not
wait for a nonexistent native function to start. Journal version 3 records this
state explicitly and can still recover version 2 records.

Then bind a device:

```powershell
# List USB devices
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

## USB versions

The transport implements USB **1.0 / 1.1 / 2.0 / 3.0 / 3.1 / 3.2** device
operation. USB4 controller/tunnel protocols are not implemented.

| Operating mode | Link rate | Virtual root hub |
|---|---:|---|
| USB 1.x low / full speed | 1.5 / 12 Mbit/s | USB2 |
| USB 2.0 high speed | 480 Mbit/s | USB2 |
| USB 3.0 / USB 3.1 Gen1 / USB 3.2 Gen1x1 | 5 Gbit/s | USB3.2 |
| USB 3.1 Gen2 / USB 3.2 Gen2x1 | 10 Gbit/s | USB3.2 |
| USB 3.2 Gen1x2 | 10 Gbit/s | USB3.2 |
| USB 3.2 Gen2x2 | 20 Gbit/s | USB3.2 |

These are USB link modes, **not measured UsbWarp throughput**. The two root hubs
share one transport and buffer pool. USB addresses and root-port numbers are
independent of the host-assigned device IDs. There are 31 USB2 ports, 15 USB3
ports, and at most 32 bound devices across both buses. Devices behind physical
hubs can be bound individually; the bridge exports individual devices, not a
physical hub/controller subtree.

Windows queries the actual parent hub using the device's driver key, then reads
EX_V2 and SSP connection information. This also works when the CLI cannot open
hub interfaces. Where Windows exposes lane information, Gen1x2, Gen2x1 and
Gen2x2 are distinguished. If that query is unavailable, the SSP category is
represented conservatively as a single-lane 10-Gbit/s virtual link. The service
returns the kernel's speed to `list`. Capability (`bcdUSB`) is not substituted
for operating speed. See Microsoft's [SSP connection information](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/usbioctl/ns-usbioctl-usb_node_connection_superspeedplus_information).

USB3 support includes BOS/SSP capabilities, extended port status, warm reset,
link-state handling, and bulk streams. Stream IDs are carried end-to-end and
mapped to the Windows stream handles with the same IDs. Groups are opened with
rollback on failure; teardown and configuration changes release the handles.
The bridge caps each endpoint at 32 nonzero streams, further limited by the
physical endpoint/controller. See [Windows static streams](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/how-to-open-streams-in-a-usb-endpoint).

Scatter-gather buffers are copied by range into/out of shared memory, including
ISO packet gaps. Bulk OUT `URB_ZERO_PACKET` sends its terminating packet before
later data on the same endpoint/stream, while independent streams stay pipelined.
Endpoint halt recovery uses the Windows pipe reset operation so both device and
host-controller state are cleared. USB3 support is negotiated with capability
bits; an older guest is never given a SuperSpeed device disguised as high speed.

A negotiated binding epoch tags requests, completions and device notifications.
A request from an old binding is rejected after its device ID is reused; old PnP
callbacks also cannot remove the replacement target. `debugfs/ring_status` exposes
current epochs so the protocol fuzzer can continue to exercise bound-device paths.

Build/software tests cover these paths. A USB 3 bulk-only storage device has
passed physical read, small-write and lifecycle tests. UAS, ISO peripherals and
SSP lane modes still require hardware validation; USB version coverage is not
a claim that every peripheral/firmware combination has been tested.

## ISO streaming

Rebuild and deploy **both Windows drivers, the service and the Linux module** together.
The controller should also be rebuilt to display USB3.x modes. Enable `--iso` on `USBWarpService.exe` and load Linux with:

```bash
sudo insmod usbwarp.ko enable_iso=1
```

The guest requires both `ISO_TRANSFER` and `ISO_PACKED` capabilities. This prevents
older experimental hosts that only advertised ISO from accepting unsupported traffic.
The normal control/bulk/interrupt protocol remains version 1.0.

- ISO IN and OUT carry per-packet lengths/statuses and the host's actual start frame.
- `SET_CONFIGURATION` and `SET_INTERFACE` use USBD configuration/interface requests,
  updating endpoint handles, intervals and bandwidth allocation, including alternate
  setting zero when a stream stops.
- The 64-byte message base is followed by up to 1,024 eight-byte packet entries:
  the maximum message size is **8,256 bytes** on both sides.
- Full-speed transfers allow 1–255 packets. High-speed, SuperSpeed and SuperSpeedPlus
  transfers allow up to 1,024,
  in complete frames: counts must be multiples of 8, 4, 2 or 1 for service intervals
  of 1, 2, 4 or at least 8 microframes, respectively. Packet sizes are checked against
  the selected Windows pipe's maximum packet size. Unsupported counts fail early;
  the bridge does not insert silent audio packets to pad a frame. These limits
  follow [Microsoft's ISO transfer contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/transfer-data-to-isochronous-endpoints).
- Linux buffer gaps are removed before submission. Windows derives OUT lengths from
  consecutive offsets, so sending the original gaps would transmit unintended bytes.
  IN completion copies only the returned bytes of each packet back to its original
  Linux offset. Per-packet errors remain per-packet errors.
- Completion messages queue when H2G is full. G2H consumption pauses under reply
  backlog. Cancellation retains shared slots until physical I/O completes; device
  removal and shutdown drain Windows I/O before telling Linux to recycle buffers.
  Module unload requests a host drain and waits for acknowledgement. Restart the
  service session before reloading the module; a stale session is rejected to
  prevent late host writes from reaching newly allocated buffers.
- Windows completions wake the host poll thread immediately; the guest uses
  MSI-X when the optional native backend has completed its handshake, with
  adaptive polling as fallback. The transport does not provide hard
  real-time guarantees or synchronized guest/physical USB frame clocks. Use
  `URB_ISO_ASAP`; explicit schedules are rejected rather than shifted silently.

The virtual PCI device declares a 64-bit address aperture while the HCD remains
CPU-copy based (`HCD_DMA` is not enabled). This prevents the Linux storage layer
from unnecessarily applying a 32-bit/SWIOTLB mapping-size restriction: on the
tested WSL kernel it otherwise split USB3 reads at 256 KiB, instead of the
normal 1 MiB limit. USB device quirks and the protocol transfer bound still apply.

For the polling path, `poll_us=0` retains adaptive polling. Values `1..1000` set the requested
empty-poll sleep interval; invalid values are rejected and ISO polling requests
are capped at 10 us. These are sleep requests, not completion deadlines: actual
wakeup includes kernel and VM scheduling. Small fixed intervals did not give a
consistent improvement in this test setup. `h2g_irq=0`, `poll_us=0` are the
defaults, retaining the original adaptive polling strategy without MSI-X
allocation. Load with `h2g_irq=1` to opt into MSI-X and allow runtime switching.
`irq_affinity=1` follows the effective MSI-X CPU on the completion worker to
avoid a second virtual CPU wakeup. Setting it to `0`, or selecting polling,
restores the worker's original CPU mask. See the [interrupt notes](docs/kernel-interrupts.txt)
for the measured latency/CPU tradeoff and interface limits.
When IRQ mode is opted into, `irq_softirq=1` drains URB/ISO completions in bounded batches in
the IRQ CPU's softirq context. Control messages stay ordered on the kernel
thread; `irq_softirq=0` selects the worker receive path for comparison.
The affinity option applies to that retained control/fallback thread.
Experimental `rx_repoll_us`/`rx_repoll_count` allow bounded, timer-driven
rechecks before re-enabling notifications, following NAPI's deferred-IRQ
pattern. `rx_repoll_us=0` remains the default: the tested WSL configuration
reduced IRQ counts but showed no net throughput/CPU improvement. See the
interrupt notes for limits and measurements.

Experimental `rx_busy_us=1..1000` enables bounded worker busy polling after
successful publication of short Control/Bulk requests. `rx_busy_bytes=128`
sets the eligible request-size limit (range 0..67108864). Successful completion
within the requested window can renew it; old or cancelled requests cannot.
Interrupt/ISO submissions do not start windows, and active ISO disables this
optimization. IRQs remain armed between drains so preemption does not leave
completions waiting exclusively for the worker. The worker and tasklet share
the same serialized consumer and management-message barrier. This first version
uses type/length hints; it does not parse device protocols or learn endpoint
profiles. On the tested U300, three interleaved samples found that a 100-us
window improved 4 KiB QD1 reads from 672.8 to 738.0 IOPS, with guest CPU time
rising from 1.81 to 3.13 seconds over the benchmark interval. Sequential QD4
p99 worsened, so `rx_busy_us=0` remains the default. These results are specific
to this device and nested VM; other USB classes still need physical tests.
See the [URB behavior and test notes](docs/urb-scheduling.txt).

The virtual HCD does not impose a blanket transfer timeout. Async receives may
legitimately remain pending indefinitely; client drivers own transfer deadlines
and unlink requests. The management worker retries explicit cancellations while
retaining shared slots until completion or DMA-safe teardown acknowledgement.

The polling default has a read-only Win11Debug stability run covering sustained
mixed reads, capture/restoration, active removal and SCM recovery. See the
[stability record](docs/polling-stability.txt) for scope, resource measurements
and limitations. IRQ comparison scripts require an IRQ-enabled module load;
they reject a polling-only load instead of labeling polling results as IRQ.

These code paths have build and software regression coverage. They have **not** yet
been validated here with a physical USB audio interface or webcam. Before claiming
hardware support, exercise capture/playback, feedback endpoints, alternate-setting
changes, simultaneous streams, cancellation and unplug while streaming.

For the Ubuntu-22.04 kernel tree used during development:

```bash
sudo ln -sfn /home/oh2/WSL2-Linux-Kernel /lib/modules/$(uname -r)/build
cd usbwarp_module
KBUILD_MODPOST_WARN=1 make
# Equivalent without modifying the build symlink:
KBUILD_MODPOST_WARN=1 make KDIR=/home/oh2/WSL2-Linux-Kernel
modinfo -F vermagic usbwarp.ko
```

A successful module build does not make it compatible with a different running
kernel. Use matching kernel sources/configuration and `Module.symvers` when loading;
`KBUILD_MODPOST_WARN=1` only downgrades missing-symbol checks to warnings.

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
| **3D** | Abnormal recovery | **pass\*** | Double bind rejected, rapid cycles recovered, mid-transfer unbind recovered |
| **4** | 22-hour endurance | **58,708 iter, zero errors** | Orphan mode recovery verified on sleep/wake |

> \*One scenario was inconclusive due to a known MicroPython CDC REPL issue where the device occasionally becomes unresponsive after reconnect. Rebinding the device restored normal operation.

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

- ✅ USB1.x–USB3.2 enumeration through separate USB2/USB3.2 root hubs
- ✅ SuperSpeed bulk streams and scatter-gather buffers (software tested)
- ✅ Full USB enumeration (GET_DESCRIPTOR, SET_CONFIGURATION, SET_INTERFACE, SET_ADDRESS)
- ✅ Control transfers (standard, class, vendor — raw setup packet passthrough)
- ✅ Bulk IN/OUT transfers (direct shared buffers on the Windows side)
- ✅ Interrupt IN transfers
- ✅ CDC ACM serial devices (tested with Raspberry Pi Pico / MicroPython)
- ✅ Device bind/unbind lifecycle with duplicate detection
- ✅ Graceful shutdown with URB cancel propagation through filter driver
- ✅ Ring corruption recovery (scan-forward + reset)
- ✅ Circuit breaker, rate limiter, and orphan detection safety layers
- ✅ SHM page locking for safe DPC-level access
- ✅ Service host/session separation, cancellable local IPC and driver-session cleanup (software tested)

**Known limitations:**

- Hardware validation covers the historical CDC ACM Pico and a SuperSpeed BOT storage device; other device classes still need hardware coverage
- ISO streaming is implemented for full-speed, high-speed, SuperSpeed and SuperSpeedPlus devices with ASAP scheduling. Hardware audio/video validation is still required; the historical Pico endurance results do not cover ISO.
- Explicit ISO frame scheduling is rejected with `-EOPNOTSUPP`; guest and physical USB frame clocks are not synchronized. Scatter-gather buffers are supported.
- Physical removal is propagated to the guest; newly inserted devices still require an explicit bind
- Single VM session only
- User-owned WSL VMs may require the service to run as the WSL owner; see the service-account instructions above
- Capture requires a connected USB leaf with a verified identity and port; hubs/controllers, disabled devices and genuine PnP start failures are rejected
- Drivers are not signed (test signing required)
- Physical ISO audio/video, UAS streams and USB3.2 dual-lane devices remain untested

## Roadmap

Planned features roughly in priority order:

- **Automatic rebind** — Reconnect selected devices after physical reinsertion
- **Additional device testing** — Extend the SuperSpeed BOT result to USB2 peripherals, UAS, audio/video and dual-lane devices
- **ISO hardware validation and frame scheduling** — Validate USB audio/webcam streams, then add clock synchronization for explicit frame schedules
- **ETW tracing** — Replace KdPrint with structured Event Tracing for Windows; sysfs stats on Linux side
- **Batch URB submission** — Coalesce multiple URBs into a single Ring message for reduced overhead
- **Configuration persistence** — Registry-based device binding that survives reboots
- **Multi-VM support** — Bind different USB devices to different WSL2 distributions simultaneously
- **Performance dashboard** — Real-time latency histograms and throughput metrics
- **Signed driver packages** — WHQL or attestation signing for deployment without test mode

## Running Tests

Windows service regression tests use the production configuration, SCM status,
driver client, HDV lifecycle, session supervisor and pipe implementation:

```powershell
.\tests\build_service_tests.ps1
# Optional real SCM integration, from an elevated shell after building:
.\x64\ReviewChecks\ServiceTests\scm_test.exe
```

The build script runs configuration/status/cancellation/native-API tests and
process/pipe tests without administrator rights. The separate SCM test creates
only `UsbWarpService.SelfTest`, installs from a path containing spaces, tests
start/stop/restart and crash recovery, then uninstalls its service/configuration.
It refuses to replace an existing test service. Both integration modes use fake
driver and VM backends; they never open real USB devices or modify real VMs.
Real SCM integration requires an elevated token; API error 5 means the test
could not create a service, not that the SCM lifecycle has been validated.

`build_service_tests.ps1 -FixtureDirectory C:\USBWarp-Test\ServiceTests -BuildOnly`
prepares the isolated SCM fixture for another machine. `kernel_test.exe` requires
the real control driver in a test VM with UsbWarpService stopped; it checks
registration, event handle validation, teardown and process-death recovery
without binding USB devices.

The Win11Debug hardware run used an assigned Intel USB 3.1 xHCI controller,
a SuperSpeed hub and an ASUS adol U300 (`346d:5678`, 5 Gbit/s), with nested
Ubuntu 22.04 on kernel 6.6.87.2. Three 64-MiB O_DIRECT reads matched an independent
Windows SHA-256 baseline at **78.76–91.18 MiB/s**. A service-process crash recovered
through SCM and the capture journal; rebind and another hash comparison passed
at **96.84 MiB/s**. The test restored the original Windows D: volume to Healthy.
These rates describe this stick and nested VM setup. No disk data was written
by the test. `tests/usb_read_test.py` checks device identity and direct reads;
`tests/vm_usb_read_test.ps1` adds capture/crash recovery and cleanup in `finally`,
with disk-identity and restored-volume checks.

The dedicated KMDF capture driver was subsequently tested in the same VM with
the same stick. During capture, the observed stack was
`UsbWarpFilter -> USBHUB3`; after release it was `USBSTOR -> USBHUB3`.
Administrator user-mode opens of the capture interface were denied. Repeated
64 MiB direct reads matched the original hash, including after SCM crash
recovery and an external port reset. The final run measured 103.90 MiB/s and
restored the original driver/model, 62,914,560,000-byte capacity and Healthy D:
volume. These short reads are not a controlled Windows-versus-Linux benchmark.

`tests/build_service_tests.ps1` includes the capture transaction fault tests
and a read-only test of real SetupAPI model selection. `capture_port_reset.cpp`
builds a VM-only helper for `-ResetRecovery` and access checks in the hardware
test. Copy its `capture_reset.exe` to the test root before using that option.

`tests/vm_driverless_read_test.ps1` additionally tests a real NULL-driver state
on the same physical USB device, including Code 28, normal release, external
port reset and forced service termination. Linux reads matched the baseline in
each case, and production restoration left no Windows INF/service/model bound.
The separate `capture_driverless_fixture.exe` saves the original native driver
in a test-only durable journal, and the script's `finally` restores that driver,
the original disk capacity and volume mount after validation. Build it with
`tests/build_service_tests.ps1` and copy it to the VM's test root before running
the script with `-Problem28 -CrashRecovery -ResetRecovery`.

The dedicated KMDF capture driver was subsequently tested in the same VM with
the same stick. During capture, the observed stack was
`UsbWarpFilter -> USBHUB3`; after release it was `USBSTOR -> USBHUB3`.
Administrator user-mode opens of the capture interface were denied. Repeated
64 MiB direct reads matched the original hash, including after SCM crash
recovery and an external port reset. The final run measured 103.90 MiB/s and
restored the original driver/model, 62,914,560,000-byte capacity and Healthy D:
volume. These short reads are not a controlled Windows-versus-Linux benchmark.

`tests/build_service_tests.ps1` includes the capture transaction fault tests
and a read-only test of real SetupAPI model selection. `capture_port_reset.cpp`
builds a VM-only helper for `-ResetRecovery` and access checks in the hardware
test. Copy its `capture_reset.exe` to the test root before using that option.

The hardware-independent regression suite also exercises the production Linux ring
implementation with RAM and pthread locks in place of MMIO/spinlocks:

```bash
bash tests/run_unit.sh
SANITIZE=1 bash tests/run_unit.sh   # AddressSanitizer and UndefinedBehaviorSanitizer
```

This checks ISO layouts, truncated/overflowing messages, packed packet boundaries,
Windows packet-count constraints, 1,024-packet messages and 40,000 concurrent ring
messages, including index rollover and backpressure. It also executes the production
root-hub, scatterlist-copy and ZLP scheduling code against deterministic test hooks,
covering USB3.2 lane modes, short descriptor reads, change-bit acknowledgement,
stream isolation, cancellation and shutdown while a ZLP is queued. It does not simulate USB DMA,
Hyper-V timing or a real device.


```bash
# Layer 1: Ring protocol (no hardware needed)
cd tests
gcc -O2 -Wall -I../include -o ring_test ring_test.c
./ring_test

# Layer 2: Protocol fuzz (requires usbwarp.ko loaded with debug=1)
sudo insmod usbwarp.ko debug=1
cd fuzz
gcc -O2 -Wall -I../../include -o ring_fuzz ring_fuzz.c
sudo ./run_fuzz.sh -v
cd ..

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
.\USBWarpService.exe --uninstall
sc stop UsbWarp
sc delete UsbWarp

# Locate the published INF name for the USBWarp capture package, then remove it.
# Service uninstall refuses to proceed while captures need restoration.
pnputil /enum-drivers
pnputil /delete-driver oemNN.inf

# Reboot
shutdown /r /t 0

# Clean up files
del C:\Windows\System32\drivers\UsbWarp.sys

# Disable test signing (optional)
bcdedit /set testsigning off
```

## License

GPL-2.0-only. See [LICENSE](LICENSE).

## Acknowledgements

Architecture informed by studying [usbipd-win](https://github.com/dorssel/usbipd-win) and [usbip-win2](https://github.com/vadimgrn/usbip-win2). UsbWarp takes a fundamentally different approach (shared memory MMIO vs. network sockets) but learned from their solutions to Windows USB stack constraints, particularly the need for a filter driver to hold a legitimate USBD_HANDLE.
