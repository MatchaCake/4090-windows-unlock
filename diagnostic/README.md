# Read-only AD102 Zotac 4090 UEFI probe (v0.4.1-ad102z)

Adapted from the upstream AD104 probe for one Zotac RTX 4090 (`10de:2684`, subsystem
`19da:4675`) on an ASUS X670E-I host. It reads PCI configuration, all BARs, the PCI
location, 22 bounded BAR0 registers, the 1 MiB physical PROM window at BAR0+0x300000,
GOP/console topology and the loaded-driver ownership graph. It writes JSON to its own
USB only; it does not unlock, write GPU MMIO, change BME, release graphics drivers or
edit boot variables.

Differences from upstream v0.4.0: target identity pins changed to 2684/19da/4675,
expected chip PMC_BOOT_0 `0x19c00a1` (AD102 A1), cached-ROM size pin relaxed to a
PCIR-validated 512 B–4 MiB range, and the capture guard accepts any UEFI-assigned PCI
location and BAR0 base (recording them) instead of the reference board's bus/dev and
`0x80000000` — the captured values get pinned in the transaction port's profile later.

Build on Windows x64 with VS 2022 Build Tools + Python 3:

```powershell
.\Build.ps1 -Python python
```

Output: `build/AD102-PROBE.EFI`. Deploy as `EFI\BOOT\BOOTX64.EFI` on a FAT32 USB stick,
cold boot, and send back the `AD102-PROBE\CAPxxxx.JSON` it produces. Captures contain
the full proprietary ROM and local topology details — keep them private.

MIT licence inherited from the upstream probe (`LICENSE.txt`); EDK II and related
notices in `THIRD_PARTY.md`.
