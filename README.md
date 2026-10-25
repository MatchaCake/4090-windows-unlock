# AD102 (RTX 4090) Windows UEFI Speed-Select Unlock

A Windows UEFI port of the SM speed-select override for one specific
**Zotac RTX 4090 (AD102 A1, `10de:2684`, subsystem `19da:4675`)**
on an **ASUS ROG STRIX X670E-I + AMD Ryzen 9 7950X3D** host.

This project restores the full FP16/BF16 tensor instruction issue rate
that consumer Ada GPUs ship with artificially halved. On the tested card,
the effective speed-select readout changed from `0x00410080` (half rate)
to `0x00000000` (full rate), and the FLR-persistent override survived
Windows driver initialization.

## Status: what is tested and what is not

| Component | Status |
|---|---|
| Transient unlock (USB boot, manual keypress `U`) | ✅ **Tested and working** on real hardware |
| iGPU-console topology release (`asus_release.c`) | ✅ Tested — one successful transaction (RUN0010) |
| Cold-profile fingerprint pins (PROM/FWSEC/registers/BARs) | ✅ Tested across 4 cold-boot captures |
| dGPU-console topology release (`asus_dgpu_release.c`) | ❌ **Untested — code only** |
| Resident / auto-unlock mode (`Build-Resident.ps1`) | ❌ **Untested — code only** |
| ESP-resident deployment + `bcdedit` boot-manager takeover | ❌ **Untested — documented design only** |

The transient unlock was validated end-to-end: the transaction opened the
four PLM permission registers via the authenticated-booter V67 payload,
wrote SS0=`0x88888888` / SS1=`0x8`, survived PCIe FLR, and the stock NVIDIA
driver initialized the card normally. However, no cuBLAS benchmark numbers
are included because the operator's actual workload (local MiniMax H3
inference with SageAttention already enabled) showed negligible improvement
from the unlock — the bottleneck is elsewhere, not in the FP16 issue rate.

The dGPU-console release and the resident/auto mode were written after the
successful transient unlock but were never exercised on hardware. They are
published as reviewable code, not as working tools.

## How the unlock works

1. A UEFI application runs before the Windows NVIDIA driver binds.
2. It validates the card's cold-state fingerprint against captured pins
   (PCI config, BAR layout, 19 register baseline values, live PROM hash,
   FWSEC descriptor geometry, boot-manager hash).
3. It disconnects any UEFI display drivers from the GPU, takes an exclusive
   PCI claim, and enables bus mastering.
4. It runs the signed FWSEC firmware from the card's own PROM on the GSP
   Falcon, establishing WPR2.
5. It starts the signed AD102 booter on SEC2 with a crafted V67 signature
   payload. The signature data redirects returns through existing
   authenticated booter instructions, which write `0xffffffff` to the four
   PLM permission registers (WPR_CFG, FBPA, WPR, FEAT_OVR_PLM).
6. The host writes SS0=`0x88888888` and SS1=`0x8` (speed-select overrides),
   verifies the effective readout is zero, and issues a PCIe FLR.
7. The override persists across FLR and stock driver initialization.
   The stock Windows NVIDIA driver then runs the card at full rate.

No NVIDIA driver code is patched. No VBIOS is flashed. The override is a
runtime register state that is lost at power-off and must be reapplied
on each cold boot.

## Repository layout

```
├── app.c                     UEFI application entry point
├── port.c / port.h           Platform layer (PCI IO, MMIO, DMA, FLR)
├── profile.c                 Card fingerprint pins (cold-state baseline)
├── checks.c                  PCI config walk and device-path validation
├── asus_release.c            iGPU-console release (TESTED)
├── asus_dgpu_release.c       dGPU-console release (UNTESTED)
├── boot_io.c                 USB/ESP log, asset loading, Windows staging
├── boot_profile.example.h    Template for the private ESP GUID header
├── compat/                   Linux kernel API compatibility shims
├── vendor/                   Transaction core (unlock_610.c et al) + EDK2 headers
├── diagnostic/               Read-only UEFI probe for capturing fingerprints
├── benchmark.py              cuBLAS GEMM benchmark (torch + ctypes)
├── compare.py                Statistical comparison of before/after JSONs
├── Build-Experimental.ps1    Build manual iGPU-console EFI (tested config)
├── Build-Resident.ps1        Build auto/resident or manual dGPU EFI (untested)
├── Build.ps1                 Core freestanding library build
├── provenance.json           Upstream commits referenced
├── LICENSE                   GPL-2.0-only
├── LICENSES/                 Upstream license notices (NVIDIA, CMP, EDK2)
└── THIRD_PARTY_NOTICES.md    Attribution
```

## Three build configurations

| Script | Output | Topology | Mode | Status |
|---|---|---|---|---|
| `Build-Experimental.ps1` | `AD102-TRANSACTION.EFI` | iGPU console | Manual (`U` keypress) | Tested |
| `Build-Resident.ps1 -Manual` | `AD102-DGPU-USB.EFI` | dGPU console | Manual (`U` keypress) | Untested |
| `Build-Resident.ps1` | `AD102-AUTO-DGPU.EFI` | dGPU console | Auto (5 s countdown) | Untested |

## Building

Requires Windows x64, MinGW-w64 GCC, Python 3, and MSVC x64 linker tools
(VS 2022 Build Tools). No GPU access or driver install occurs during build.

```powershell
# Clone and enter
cd ad102-zotac

# Create the private ESP profile (not committed)
New-Item -ItemType Directory -Force local
Copy-Item boot_profile.example.h local/boot_profile.h
# Edit local/boot_profile.h and replace the zero GUID with your ESP partition GUID

# Build the tested manual iGPU-console EFI
.\Build-Experimental.ps1
# → build\AD102-TRANSACTION.EFI

# Or build the untested dGPU-console / resident variants
.\Build-Resident.ps1 -Manual   # → build\AD102-DGPU-USB.EFI
.\Build-Resident.ps1           # → build\AD102-AUTO-DGPU.EFI
```

Firmware assets (5 files, ~85 MiB total) must be generated locally from the
pinned upstream workflow — see the [parent repository](https://github.com/JustAResearcher/gpu-unlock-ada)
for `make assets`. These files are NVIDIA proprietary firmware and are not
distributed in this repository.

## Deploying the tested transient unlock

1. Prepare a FAT32 USB stick:
   - `EFI\BOOT\BOOTX64.EFI` = `AD102-TRANSACTION.EFI`
   - `AD102-ASSETS\` = the 5 generated firmware `.bin` files
   - Connect the monitor to the motherboard's iGPU output
2. Disable Secure Boot in BIOS.
3. Fully power off, wait 10 s, boot via F8 → UEFI USB.
4. First boot: press `S` (safe rehearsal — validates all pins without
   touching the GPU). Check `AD102-UNLOCK\RUN0001.TXT`.
5. Second cold boot: press `U` (real transaction). Screen freezes briefly
   during release, Windows re-lights it.

## Why the operator stopped here

The unlock was technically successful — the transaction completed, all
hardware readbacks passed, and Windows booted normally. However, the
operator's primary use case is local inference of MiniMax H3, which already
uses SageAttention for attention kernel optimization. With SageAttention
enabled, the unlock provided negligible end-to-end speedup because the
FP16/BF16 tensor issue-rate bottleneck had already been bypassed by the
attention kernel implementation. The cost of maintaining a boot-time
unlock routine outweighed the marginal gain for this specific workload.

The code is published in the hope that it is useful to others whose
workloads do benefit from the full tensor issue rate (e.g., workloads
without fused attention kernels, or prefill-heavy inference where the
raw GEMM rate matters).

## Credits and upstream projects

This port stands on the shoulders of the following projects and people:

### Parent repositories

- **[JustAResearcher/gpu-unlock-ada](https://github.com/JustAResearcher/gpu-unlock-ada)** —
  The Linux kernel module for RTX 4090 and other Ada GPUs, the AD104 MSI
  Windows reference port (`windows/ad104-msi/`), and the firmware generation
  pipeline. The `vendor/` transaction core in this repository is adapted
  from their kernel module with device admission changed from `0x2782`
  to `0x2684`.

- **[Zagitta/gpu-unlock](https://github.com/Zagitta/gpu-unlock)** —
  The original Ampere (RTX 30-series) Linux kernel module. The fundamental
  approach (pre-driver FWSEC + V67 payload + SS0/SS1 override + FLR)
  originates here.

- **[TOTORONG/AD103_win_unlocker](https://huggingface.co/TOTORONG/AD103_win_unlocker)** —
  The AD102 four-PLM payload and its published debug-booter analysis.
  Without this work, the Ada unlock would not exist.

### Windows UEFI strategy references

- **[impacifist/Windows-GPU-UNLOCK](https://github.com/impacifist/Windows-GPU-UNLOCK)** —
  The RTX 3090 Windows UEFI port whose snapshot/delta release strategy
  and boot-manager chainload design informed the ownership-handling
  approach in this port.

- **Latence.app / [Bizcott](https://github.com/Bizcott)** —
  The AD104 MSI RTX 4070 Ti Windows reference port (`windows/ad104-msi/`)
  that this project is directly adapted from. Their contribution includes
  the UEFI PCI IO / DMA / error-retention state machine, console ownership
  capture and callback-based release, exclusive PCI claim logic, USB
  containment rules, and the 177-scenario test suite. This AD102 port
  is a derivative of their work with device admission and topology
  changed for the Zotac 4090 / ASUS X670E-I target.

### Additional referenced work

- **[WildFlash1st/cmp90hx-unlock](https://github.com/WildFlash1st/cmp90hx-unlock)** —
  The CMP GA102 V67 chain that underpins the Ampere unlock.

- **NVIDIA open-gpu-kernel-modules** — Published loader code, GSP firmware
  format, and Falcon architecture documentation that made the reverse
  engineering possible.

- **[envytools](https://github.com/envytools/envytools)** — Falcon FUC5
  disassembler used for the booter instruction analysis.

### Operator's port contributions (this repository)

- Adapted all fingerprint pins from the AD104 MSI reference to the Zotac
  4090 / ASUS X670E-I (PCI IDs, BAR layout, 19 cold-state register values,
  PROM hash, FWSEC geometry, ESP partition and boot-manager hash).
- Rewrote the ownership-release module for the iGPU-console topology
  (`asus_release.c`) — simplified from the AD104 reference because the
  console runs on the iGPU, not the target GPU.
- Wrote (but did not test) a dGPU-console multi-stage release
  (`asus_dgpu_release.c`) and resident/auto-unlock mode.
- Removed the upstream reference board's SS1==1 pin from the vendor
  transaction core (the AD102 Zotac's stock SS1 is connector-state
  dependent).
- Added per-pin failure logging to the cold-profile verifier.

## License

GPL-2.0-only. NVIDIA firmware remains under its own terms. See
[LICENSE](LICENSE), [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md),
and [LICENSES/](LICENSES/) for the full notice chain.

## Safety and disclaimer

This software writes to GPU hardware registers and halts on failure.
Recovery from a halted state requires a full power-off. There is no
guarantee of fitness for any purpose. The tested configuration (manual
iGPU-console) completed one successful transaction with no adverse
effects; all other configurations are untested code. Use at your own risk.

**THIS REPOSITORY IS PUBLISHED FOR RESEARCH AND EDUCATIONAL PURPOSES ONLY.**

The author provides this code "AS IS", without warranty of any kind,
express or implied, including but not limited to the warranties of
merchantability, fitness for a particular purpose, and noninfringement.
In no event shall the author be liable for any claim, damages, or other
liability, whether in an action of contract, tort, or otherwise, arising
from, out of, or in connection with this software or the use or other
dealings in this software.

**By using any code from this repository, you acknowledge that:**

- You are solely responsible for any damage to your hardware, software,
  data, or any other consequences that may result from running this code.
- This code writes to proprietary GPU registers that NVIDIA does not
  document for end-user modification. Doing so may void your warranty,
  violate NVIDIA's EULA, or cause irreversible hardware damage.
- The author has tested this on exactly one card (Zotac RTX 4090) on
  exactly one motherboard (ASUS ROG STRIX X670E-I) and makes no claim
  that it will work, or even fail safely, on any other hardware.
- If your computer stops booting, displays a black screen, corrupts data,
  or catches fire — that is your problem, not the author's.

If these terms are unacceptable, do not use this code.
