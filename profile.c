/* SPDX-License-Identifier: GPL-2.0-only */
/* Exact captured Zotac AD102 RTX 4090 (10de:2684 / 19da:4675) on ASUS ROG
 * STRIX X670E-I, iGPU-console topology. Pins derive from the AD102.rom dump
 * analysis, the CAP0003/CAP0004 cold captures (byte-identical pair) and the
 * host ESP inventory. A matched profile is not hardware qualification.
 * v0.6.1: every pin mismatch now names itself in the RUN log (diagnostic
 * instrumentation only; pin values and behavior are unchanged). */
#include "profile.h"
#include "checks.h"
#include "sha256.h"
#include "compat/compat.h"

static const struct {
  const char *name,*hash;
  UINTN bytes;
} Pins[5]={
 {"gpu-unlock/610.43.03/booter_load_ad102.bin","6ee05b20e1dc244c08c052f36ef28469b8d09f4955d1fe33e48a179e11a7fd49",57672},
 {"gpu-unlock/610.43.03/v67-ad102-610.43.03-four-plm.bin","44c36b2d6b3424e53e18f3e55db85c833655de071ce66ce42749588a4a7dc8cb",64000},
 {"gpu-unlock/610.43.03/gsp_ga10x.bin","572373620a37418f24dc16b5031c39338778c3257e48e8408de9a57291b24f3a",84277400},
 {"gpu-unlock/610.43.03/gsp_boot_ad102.bin","45d6335a8185d6ff13e8d1484cea73f68d3d13aa1c19dc080ed1c3482dd5bac0",36864},
 {"gpu-unlock/610.43.03/gsp_boot_desc_ad102.bin","83f8f3f1f5f9496e0ea8fc7103c2beb64d0b6de2f5ffe725f13514de5a95fa42",84}
};
BOOLEAN GuAd102AssetsValid(VOID) {
  for(UINTN i=0;i<5;i++)
    if(!GuAssets[i].name || strcmp(GuAssets[i].name,Pins[i].name) ||
       GuAssets[i].firmware.size!=Pins[i].bytes ||
       !GuHashMatches(GuAssets[i].firmware.data,Pins[i].bytes,Pins[i].hash)) {
      GuLog("AD102 pin: asset %zu mismatch (named=%u size=%zu want %zu)\n",
        (size_t)i,GuAssets[i].name?1u:0u,(size_t)GuAssets[i].firmware.size,(size_t)Pins[i].bytes);
      return FALSE;
    }
  return TRUE;
}
/* Live 1 MiB PROM window at BAR0+0x300000, measured twice (CAP0003/CAP0004). */
BOOLEAN GuAd102PromMatches(const VOID *Data,UINTN Bytes) {
  return Bytes==1048576 && GuHashMatches(Data,Bytes,
    "6424c9c86a28b328d478203a84ab3489340ad60ca8f6fc9a145748cf89077313");
}
/* FWSEC descriptor on the live PROM chain (dump- and capture-verified). */
BOOLEAN GuAd102FwsecMatches(const struct gpu_unlock_fwsec *f) {
  return f && f->image_off==0x44168 && f->image_size==0x10000 &&
    f->imem_pa==0 && f->imem_size==0xf200 && f->imem_va==0 &&
    f->dmem_pa==0 && f->data_off==0xf200 && f->dmem_size==0xd80 &&
    f->pkc_off==0xb24 && f->interface_off==0x1c && f->ucode_id==9 &&
    f->engine_mask==0x400 && f->sig_count==2 && f->sig_versions==3 &&
    f->sig_off==0x43e68 && f->sig_bytes==768;
}

static BOOLEAN Config(VOID) {
  UINT32 c[64];UINTN s,b,d,f;GU_CAPS caps;
  if(!Gu.pci || !Gu.pci->GetLocation || !Gu.pci->Pci.Read ||
     Gu.bar_base!=GU_BAR0_BASE || Gu.bar_size!=0x1000000 ||
     EFI_ERROR(Gu.pci->GetLocation(Gu.pci,&s,&b,&d,&f))) {
    GuLog("AD102 pin: pci protocol or platform BAR0 state invalid\n");return FALSE;
  }
  for(UINTN i=0;i<64;i++)
    if(EFI_ERROR(GuConfigRead(EfiPciIoWidthUint32,(UINT32)i*4,&c[i])))return FALSE;
  if(!GuCheckConfig(c,s,b,d,f,&caps)) {
    GuLog("AD102 pin: config walk failed seg=%zu bus=%zu dev=%zu fun=%zu cmd=%#x\n",
      (size_t)s,(size_t)b,(size_t)d,(size_t)f,c[1]);
    return FALSE;
  }
  /* (dword offset, expected): class/rev 030000a1; BAR0=0xdd000000 (32b);
   * BAR1=0xf000000000 (64b); BAR3=0xf800000000 (64b); BAR5=IO 0xf000. */
  static const UINT8 idx[]={2,4,5,6,7,8,9};
  static const UINT32 want[]={0x030000a1u,0xdd000000u,0x0000000cu,0x000000f0u,
    0x0000000cu,0x000000f8u,0x0000f001u};
  for(UINTN i=0;i<sizeof(idx)/sizeof(idx[0]);i++)
    if(c[idx[i]]!=want[i]) {
      GuLog("AD102 pin: config[%u]=%#x expected %#x\n",(unsigned)(idx[i]*4),c[idx[i]],want[i]);
      return FALSE;
    }
  if(caps.pm!=Gu.pm_cap || caps.pcie!=Gu.pcie_cap || caps.pcie_version!=Gu.pcie_version ||
     caps.msi!=Gu.msi_cap || caps.msix!=Gu.msix_cap) {
    GuLog("AD102 pin: capability identity changed pm=%#x pcie=%#x v=%u msi=%#x msix=%#x\n",
      caps.pm,caps.pcie,caps.pcie_version,caps.msi,caps.msix);
    return FALSE;
  }
  return TRUE;
}
static BOOLEAN Registers(UINT32 *Ss0,UINT32 *Ss1) {
  static const char *const names[]={"PMC_BOOT_0","GFW_PLM","GFW_BOOT","GFW_CPUCTL",
    "WPR2_LO","WPR2_HI","WPR_CFG_PLM","WPR_PLM","FBPA_PLM","FEAT_PLM","FEAT_READOUT_0",
    "FEAT_READOUT_1","FB_SIZE_MB","VGA_BASE","MMU_PLM","MMU_LO","MMU_HI",
    "BOOTER_UCODE_FUSE","FWSEC_UCODE_FUSE"};
  static const UINT32 offsets[]={0,0x118128,0x118234,0x110100,0x1fa824,0x1fa828,
    0x1fa7cc,0x1fa7c4,0x9a0148,0x823804,0x823814,0x823818,0x1183a4,0x625f04,0x1fa7c8,0x1fa82c,0x1fa830,0x824148,0x8241e0};
  static const UINT32 values[]={0x192000a1,0x8b8f,0x3ff,0x10,0x1ffffe00,0,
    0x4cb8f,0x4cb8f,0xffffff0f,0xffffff8f,0xee018233,0x410080,0x59f4,1,0x45cb8f,0x100000,0x56f3ff0,1,1};
  for(UINTN i=0;i<sizeof(offsets)/sizeof(offsets[0]);i++) {
    UINT32 v=GuRead32(offsets[i]);
    if(Gu.fault){GuLog("AD102 pin: MMIO read fault at %#x\n",offsets[i]);return FALSE;}
    if(v!=values[i]) {
      GuLog("AD102 pin: %s(%#x)=%#x expected %#x\n",names[i],offsets[i],v,values[i]);
      return FALSE;
    }
  }
  *Ss0=GuRead32(0x82381c);*Ss1=GuRead32(0x823820);
  if(Gu.fault){GuLog("AD102 pin: MMIO read fault reading selectors\n");return FALSE;}
  GuLog("AD102 entry selectors: ss0=%#x ss1=%#x\n",(unsigned)*Ss0,(unsigned)*Ss1);
  /* Entry selectors are connector-state dependent on this board (observed stock
   * entries: 0x00724033/0, 0x00734013/1, 0x00724013/0); the upstream latch
   * design accepts any stock entry. Stock-ness itself is pinned by the fixed
   * table (readout 0x00410080 half-rate, FEAT_PLM locked) plus SS0 not already
   * unlocked and SS1 not already at the target selector. */
  if(*Ss0==0xffffffff || (*Ss0&0xffff0000)==0xbadf0000 || *Ss0==0x88888888) {
    GuLog("AD102 pin: SS0 sanity refused (%#x)\n",(unsigned)*Ss0);return FALSE;
  }
  if(*Ss1==0x8) {
    GuLog("AD102 pin: SS1 already at unlocked selector 8\n");return FALSE;
  }
  if(*Ss1>0xf) {
    GuLog("AD102 pin: SS1 out of range (%#x)\n",(unsigned)*Ss1);return FALSE;
  }
  return TRUE;
}
BOOLEAN GuAd102ColdEntry(VOID) {
  UINT32 ss0,ss1;
  return !Gu.started && !Gu.fault && !Gu.retained && Config() &&
    Registers(&ss0,&ss1) && GuEntrySelectorsMatch(ss0,ss1);
}
EFI_STATUS GuAd102Prepare(VOID) {
  UINT32 ss0,ss1,after0,after1;VOID *prom=NULL;struct gpu_unlock_fwsec fw;
  if(Gu.started || Gu.fault || Gu.retained || Gu.allocations || Gu.profile_verified ||
     Gu.prom_verified) {
    GuLog("AD102 state refused prepare: started=%u fault=%u retained=%u allocs=%zu pv=%u promv=%u\n",
      Gu.started,Gu.fault,Gu.retained,(size_t)Gu.allocations,Gu.profile_verified,Gu.prom_verified);
    return EFI_NOT_READY;
  }
  if(!Config())return EFI_NOT_READY;
  if(!Registers(&ss0,&ss1))return EFI_NOT_READY;
  if(!GuAd102AssetsValid())return EFI_CRC_ERROR;
  prom=GuAlloc(1048576);if(!prom)return EFI_OUT_OF_RESOURCES;
  GuCopyFromIo(prom,(VOID *)(UINTN)(Gu.bar_base+0x300000),1048576);
  BOOLEAN valid=!Gu.fault && GuAd102PromMatches(prom,1048576) &&
    !gpu_unlock_vbios_fwsec(prom,1048576,&fw) && GuAd102FwsecMatches(&fw);
  if(valid) {
    valid=Config() && Registers(&after0,&after1) && ss0==after0 && ss1==after1;
    if(!valid)GuLog("AD102 pin: state changed during PROM read\n");
  } else {
    UINT8 hash[32];
    if(Gu.fault)GuLog("AD102 pin: PROM window read fault\n");
    else {
      GuSha256(prom,1048576,hash);
      GuLog("AD102 pin: live PROM sha256=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x\n",
        hash[0],hash[1],hash[2],hash[3],hash[4],hash[5],hash[6],hash[7],hash[8],hash[9],hash[10],hash[11],
        hash[12],hash[13],hash[14],hash[15],hash[16],hash[17],hash[18],hash[19],hash[20],hash[21],hash[22],
        hash[23],hash[24],hash[25],hash[26],hash[27],hash[28],hash[29],hash[30],hash[31]);
      if(!gpu_unlock_vbios_fwsec(prom,1048576,&fw))
        GuLog("AD102 pin: live FWSEC off=%#x size=%#x imem=%#x dmem=%#x pkc=%#x iface=%#x ucode=%u engine=%#x sigs=%u/%#x at=%#x bytes=%u\n",
          fw.image_off,fw.image_size,fw.imem_size,fw.dmem_size,fw.pkc_off,fw.interface_off,
          fw.ucode_id,fw.engine_mask,fw.sig_count,fw.sig_versions,fw.sig_off,fw.sig_bytes);
      else GuLog("AD102 pin: live PROM chain parse failed\n");
    }
  }
  GuFree(prom);
  if(!valid)return EFI_COMPROMISED_DATA;
  Gu.prom_verified=TRUE;
  if(!GuLatchEntrySelectors(ss0,ss1)){GuLog("AD102 pin: selector latch refused\n");Gu.prom_verified=FALSE;return EFI_NOT_READY;}
  Gu.profile_verified=TRUE;
  GuLog("AD102 cold profile verified: all pins green (ss0=%#x ss1=%#x)\n",(unsigned)ss0,(unsigned)ss1);
  return EFI_SUCCESS;
}
