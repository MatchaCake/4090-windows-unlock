/* SPDX-License-Identifier: GPL-2.0-only */
/* ASUS X670E-I / Zotac AD102 ownership adapter, dGPU-console topology.
 * Compiled in place of asus_release.c when GU_TOPOLOGY_DGPU is defined.
 * Structure follows the upstream MSI Z490/AD104 adapter (which adapted
 * impacifist/Windows-GPU-UNLOCK 9352abd): the UEFI console runs through the
 * 4090's own GOP, so the release must peel the console stack off the card
 * before the firmware transaction. CAP0001/CAP0002 (display on the 4090,
 * byte-identical pair) pin the exact graph:
 *   PCI handle: PCI_IO BY_DRIVER (nvgop-ad1xx) + DevicePath BY_CHILD (GOP child)
 *   GOP child : TextOut BY_DRIVER (ConSplitter 436) + TextOut BY_CHILD(console)
 *               + GOP BY_DRIVER (GraphicsConsole 441) + EdidDiscovered
 *               BY_DRIVER (157) — screen freezes at stage 1, dies at stage 4,
 *               Windows re-lights it. No driver code/data patch, no firmware
 *               variables, no controller reconnect, no reset fallback. */
#include "asus_release.h"
#include "boot_io.h"
#include "profile.h"
#include "sha256.h"
#include "compat/compat.h"
#include <Protocol/GraphicsOutput.h>
#include <Protocol/DriverBinding.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/SimpleTextOut.h>

#define MAX_EDGES 4096
typedef struct {EFI_HANDLE handle,agent,controller;EFI_GUID protocol;UINT32 attributes,count;} EDGE;
typedef struct {UINTN count;EDGE edges[MAX_EDGES];} GRAPH;
typedef struct {
  EFI_SYSTEM_TABLE *table;EFI_HANDLE self,pci,gop,nv_agent,out_agent,gfx_agent,edid_agent;
  EFI_HANDLE console,input,error;
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *text,*err;
  EFI_SIMPLE_TEXT_INPUT_PROTOCOL *keys;
  UINTN key_reset,key_read;EFI_EVENT key_wait;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *gop_interface;
  EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE mode;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION info;
  UINT32 config[64];UINT64 attributes;
  UINTN stage;BOOLEAN ready,mutated,claimed;
} PLAN;
static PLAN Plan;
static GRAPH Original,Current;
/* Raw 16-byte GUIDs in memory order (capture-verified, no endianness guessing). */
static const UINT8 PciIoGuid[16]  ={0x00,0xb2,0xf5,0x4c,0xb8,0x68,0xa5,0x4c,0x9e,0xec,0xb2,0x3e,0x3f,0x50,0x02,0x9a};
static const UINT8 DevPathGuid[16]={0x91,0x6e,0x57,0x09,0x3f,0x6d,0xd2,0x11,0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b};
static const UINT8 TextOutGuid[16]={0xc2,0x77,0x74,0x38,0xc7,0x69,0xd2,0x11,0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b};
static const UINT8 GopGuid[16]    ={0xde,0xa9,0x42,0x90,0xdc,0x23,0x38,0x4a,0x96,0xfb,0x7a,0xde,0xd0,0x80,0x51,0x6b};
static const UINT8 EdidGuid[16]   ={0xf6,0x34,0x0c,0x1c,0x80,0xd3,0xfa,0x41,0xa0,0x49,0x8a,0xd0,0x6c,0x1a,0x66,0xaa};
static EFI_GUID PciGuid=EFI_PCI_IO_PROTOCOL_GUID,PathGuid=EFI_DEVICE_PATH_PROTOCOL_GUID;
static EFI_GUID GopGuid2=EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
static EFI_GUID GlobalGuid={0x8be4df61,0x93ca,0x11d2,{0xaa,0x0d,0,0xe0,0x98,0x03,0x2b,0x8c}};
static EFI_GUID BindingGuid=EFI_DRIVER_BINDING_PROTOCOL_GUID,ImageGuid=EFI_LOADED_IMAGE_PROTOCOL_GUID;
/* 4090 PCI handle: PNP0A03 / PCI(F1,D1) / PCI(F0,D0). */
static const UINT8 TargetPath[]={2,1,12,0,0xd0,0x41,3,10,0,0,0,0,1,1,6,0,1,1,1,1,6,0,0,0,127,255,4,0};
/* Its GOP child + ConOut route: ... / ADR(0x80010300); 1920x1080 fb=BAR1. */
static const UINT8 DgopPath[]={2,1,12,0,0xd0,0x41,3,10,0,0,0,0,1,1,6,0,1,1,1,1,6,0,0,0,
  2,3,8,0,0x00,0x03,0x01,0x80,127,255,4,0};
/* Loaded-image file-path nodes of the four pinned drivers (CAP0001). */
static const UINT8 OutFile[] ={4,6,20,0,0x7d,0x49,0x8a,0x62,0xf6,0x2b,0x64,0x42,0x87,0x41,0x06,0x9d,0xbd,0x33,0x99,0xd6,127,255,4,0};
static const UINT8 GfxFile[] ={4,6,20,0,0xdd,0xab,0xe7,0x43,0x52,0xe3,0xfb,0x4c,0xa2,0x30,0x4c,0xdc,0x1d,0x35,0x0e,0x5c,127,255,4,0};
static const UINT8 EdidFile[]={4,6,20,0,0x0c,0x03,0xa7,0xc7,0xd8,0xc3,0xee,0x45,0xbe,0xd9,0x5d,0x9e,0x76,0x76,0x29,0x53,127,255,4,0};
static const UINT8 NvFile[] ={4,4,28,0,'n',0,'v',0,'g',0,'o',0,'p',0,'-',0,'a',0,'d',0,'1',0,'x',0,'x',0,0,0,127,255,4,0};

static BOOLEAN Equal(const VOID *a,const VOID *b,UINTN n){return !memcmp(a,b,n);}
/* EFI_GUID is 16 raw bytes in memory: compare against capture-order byte arrays. */
static BOOLEAN RawGuid(const EDGE *e,const UINT8 raw[16]){return Equal(&e->protocol,raw,16);}
static EFI_STATUS Interface(EFI_HANDLE h,EFI_GUID *g,VOID **p) {
  *p=NULL;EFI_STATUS s=Plan.table->BootServices->HandleProtocol(h,g,p);
  return s==EFI_SUCCESS&&!*p?EFI_COMPROMISED_DATA:s;
}
static BOOLEAN Path(EFI_HANDLE h,const UINT8 *bytes,UINTN count) {
  EFI_DEVICE_PATH_PROTOCOL *p=NULL;
  if(Interface(h,&PathGuid,(VOID **)&p)!=EFI_SUCCESS)return FALSE;
  UINTN at=0;
  while(at+4<=count) {
    UINTN n=p->Length[0]+((UINTN)p->Length[1]<<8);
    if(n<4||n>count-at||!Equal(p,bytes+at,n))return FALSE;
    if(p->Type==END_DEVICE_PATH_TYPE)return n==4&&p->SubType==END_ENTIRE_DEVICE_PATH_SUBTYPE&&at+4==count;
    at+=n;p=(VOID *)((UINT8 *)p+n);
  }
  return FALSE;
}
static EFI_STATUS Snapshot(GRAPH *g) {
  EFI_BOOT_SERVICES *b=Plan.table->BootServices;EFI_HANDLE *handles=NULL;UINTN count=0;g->count=0;
  EFI_STATUS s=b->LocateHandleBuffer(AllHandles,NULL,NULL,&count,&handles);
  if(s==EFI_SUCCESS&&(!handles||!count||count>4096))s=EFI_BAD_BUFFER_SIZE;
  for(UINTN i=0;s==EFI_SUCCESS&&i<count;i++) {
    EFI_GUID **protocols=NULL;UINTN pc=0;
    s=b->ProtocolsPerHandle(handles[i],&protocols,&pc);
    if(s==EFI_SUCCESS&&(!protocols||!pc||pc>64))s=EFI_BAD_BUFFER_SIZE;
    for(UINTN j=0;s==EFI_SUCCESS&&j<pc;j++) {
      EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *entries=NULL;UINTN ec=0;
      if(!protocols[j]){s=EFI_COMPROMISED_DATA;break;}
      s=b->OpenProtocolInformation(handles[i],protocols[j],&entries,&ec);
      if(s==EFI_SUCCESS&&(ec>128||(ec&&!entries)))s=EFI_BAD_BUFFER_SIZE;
      for(UINTN k=0;s==EFI_SUCCESS&&k<ec;k++) {
        EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *e=&entries[k];
        if(e->Attributes==1||e->Attributes==2||e->Attributes==4)continue;
        BOOLEAN own=Plan.claimed&&handles[i]==Plan.pci&&e->AgentHandle==Plan.self&&
          !e->ControllerHandle&&e->Attributes==EFI_OPEN_PROTOCOL_EXCLUSIVE&&e->OpenCount==1;
        if(g->count==MAX_EDGES||!e->AgentHandle||(!e->ControllerHandle&&!own)||!e->OpenCount){s=EFI_COMPROMISED_DATA;break;}
        g->edges[g->count++]=(EDGE){handles[i],e->AgentHandle,e->ControllerHandle,*protocols[j],e->Attributes,e->OpenCount};
      }
      if(entries&&b->FreePool(entries)!=EFI_SUCCESS)s=EFI_DEVICE_ERROR;
    }
    if(protocols&&b->FreePool(protocols)!=EFI_SUCCESS)s=EFI_DEVICE_ERROR;
  }
  if(handles&&b->FreePool(handles)!=EFI_SUCCESS)s=EFI_DEVICE_ERROR;
  return s;
}
static BOOLEAN SameEdge(const EDGE *a,const EDGE *b) {
  return a->handle==b->handle&&a->agent==b->agent&&a->controller==b->controller&&
    a->attributes==b->attributes&&a->count==b->count&&Equal(&a->protocol,&b->protocol,16);
}
/* Stage order mirrors the ownership stack: 1 ConSplitter(TextOut on GOP),
 * 2 GraphicsConsole(GOP on GOP), 3 EdidDiscovered consumer, 4 nvgop(PCI). */
static BOOLEAN Removed(const EDGE *e,UINTN stage) {
  if(stage>=1&&e->handle==Plan.gop&&e->agent==Plan.out_agent&&RawGuid(e,TextOutGuid))return TRUE;
  if(stage>=2&&e->handle==Plan.gop&&e->agent==Plan.gfx_agent&&RawGuid(e,GopGuid))return TRUE;
  if(stage>=3&&e->handle==Plan.gop&&e->agent==Plan.edid_agent&&RawGuid(e,EdidGuid))return TRUE;
  return stage>=4&&e->agent==Plan.nv_agent;
}
static BOOLEAN ProtectedHandle(EFI_HANDLE h) {
  return h&&(h==Plan.pci||h==Plan.gop||h==Plan.self||h==Plan.nv_agent||
    h==Plan.out_agent||h==Plan.gfx_agent||h==Plan.edid_agent||
    h==Plan.console||h==Plan.input||h==Plan.error);
}
static BOOLEAN ProtectedEdge(const EDGE *e) {
  return ProtectedHandle(e->handle)||ProtectedHandle(e->agent)||ProtectedHandle(e->controller);
}
static BOOLEAN OwnEdge(const EDGE *e) {
  return e->handle==Plan.pci&&e->agent==Plan.self&&!e->controller&&
    RawGuid(e,PciIoGuid)&&e->attributes==EFI_OPEN_PROTOCOL_EXCLUSIVE&&e->count==1;
}
static BOOLEAN KnownEdge(const EDGE *e,UINTN stage) {
  for(UINTN i=0;i<Original.count;i++)
    if(!Removed(&Original.edges[i],stage)&&SameEdge(&Original.edges[i],e))return TRUE;
  return FALSE;
}
static BOOLEAN Expected(UINTN stage,BOOLEAN claimed) {
  UINTN own=0;
  for(UINTN i=0;i<Original.count;i++) {
    if(Removed(&Original.edges[i],stage))continue;
    UINTN matches=0;
    for(UINTN j=0;j<Current.count;j++)if(SameEdge(&Original.edges[i],&Current.edges[j]))matches++;
    if(matches!=1)return FALSE;
  }
  for(UINTN j=0;j<Current.count;j++) {
    const EDGE *e=&Current.edges[j];
    if(OwnEdge(e)){own++;continue;}
    if(!KnownEdge(e,stage)&&ProtectedEdge(e))return FALSE;
    for(UINTN k=j+1;k<Current.count;k++)if(SameEdge(e,&Current.edges[k]))return FALSE;
  }
  return own==(claimed?1u:0u);
}
#define NOTE(...) do{if(GuBootLogOpen())GuLog(__VA_ARGS__);}while(0)
static VOID NoteEdge(const char *kind,const EDGE *e) {
  const UINT8 *g=(const UINT8 *)&e->protocol;
  NOTE("AD102 graph %s: handle=%#llx agent=%#llx controller=%#llx protocol=%02x%02x%02x%02x attributes=%#x count=%u\n",kind,
    (unsigned long long)(UINTN)e->handle,(unsigned long long)(UINTN)e->agent,(unsigned long long)(UINTN)e->controller,
    g[0],g[1],g[2],g[3],e->attributes,e->count);
}
static VOID GraphDiff(BOOLEAN claimed) {
  for(UINTN i=0;i<Original.count;i++) {
    if(Removed(&Original.edges[i],4))continue;
    UINTN matches=0;
    for(UINTN j=0;j<Current.count;j++)if(SameEdge(&Original.edges[i],&Current.edges[j]))matches++;
    if(matches!=1){NoteEdge(matches?"duplicate":"missing",&Original.edges[i]);return;}
  }
  for(UINTN j=0;j<Current.count;j++) {
    const EDGE *e=&Current.edges[j];
    if(!KnownEdge(e,4)&&!(claimed&&OwnEdge(e))&&ProtectedEdge(e)){NoteEdge("unexpected",e);return;}
    for(UINTN k=j+1;k<Current.count;k++)if(SameEdge(e,&Current.edges[k])){NoteEdge("duplicate",e);return;}
  }
}
/* The four drivers this port may disconnect, pinned as captured. */
static EFI_STATUS Driver(UINTN which) {
  EFI_HANDLE a=which==0?Plan.out_agent:which==1?Plan.gfx_agent:which==2?Plan.edid_agent:Plan.nv_agent;
  EFI_DRIVER_BINDING_PROTOCOL *d=NULL;EFI_LOADED_IMAGE_PROTOCOL *im=NULL;
  EFI_STATUS s=Interface(a,&BindingGuid,(VOID **)&d);
  if(s!=EFI_SUCCESS||!d->Stop||d->DriverBindingHandle!=a||!d->ImageHandle||
     d->ImageHandle!=a)return EFI_UNSUPPORTED;
  if(d->Version!=(which==3?0x00070009u:which==2?0x00000001u:0x00000010u))return EFI_UNSUPPORTED;
  s=Interface(d->ImageHandle,&ImageGuid,(VOID **)&im);if(s!=EFI_SUCCESS)return s;
  UINTN size=which==0?30752:which==1?18592:which==2?36864:161696;
  UINTN stop=which==0?0x1494:which==1?0xf04:which==2?0x157c:0x6ec;
  UINTN code=which==2?0x1000:0x280,bytes=which==0?24957:which==1?10545:which==2?14584:137439;
  const char *hash=which==0?"0cafd507ffb777fa9e014f8463da040d3672b33905a74aee476da88abf060a90":
    which==1?"1953b188db1fe27f397402ae5bbb0a244e184fb9adb9604615f0d467a9a0d084":
    which==2?"53ef1716fa7a8cf866c986e85dd8412185a9fd58dd89e90202f97cb0997c96a6":
    "1905d2b2d7e2022a36d1aba5e3f26faa085ea9966c0ce15cb6c1e10cf72d6f71";
  const UINT8 *file=which==0?OutFile:which==1?GfxFile:which==2?EdidFile:NvFile;
  UINTN file_bytes=which==3?sizeof(NvFile):sizeof(OutFile);
  if(!im->ImageBase||im->ImageSize!=size||im->ImageCodeType!=EfiBootServicesCode||
     (UINTN)d->Stop!=(UINTN)im->ImageBase+stop||!im->FilePath||
     !Equal(im->FilePath,file,file_bytes)||code+bytes>size)return EFI_UNSUPPORTED;
  if(which==3&&(im->DeviceHandle!=Plan.pci))return EFI_UNSUPPORTED;
  return GuHashMatches((UINT8 *)im->ImageBase+code,bytes,hash)?EFI_SUCCESS:EFI_CRC_ERROR;
}
/* Console runs through the 4090's GOP: pin ConOut + the single GOP + mode. */
static EFI_STATUS ConsoleRoute(BOOLEAN first) {
  EFI_BOOT_SERVICES *b=Plan.table->BootServices;
  UINT8 buffer[128];UINTN size=sizeof(buffer);UINT32 attributes=0;
  EFI_STATUS s=Plan.table->RuntimeServices->GetVariable(L"ConOut",&GlobalGuid,&attributes,&size,buffer);
  if(s!=EFI_SUCCESS||size!=sizeof(DgopPath)||memcmp(buffer,DgopPath,size))return EFI_NOT_READY;
  EFI_HANDLE *h=NULL;UINTN count=0;
  s=b->LocateHandleBuffer(ByProtocol,&GopGuid2,NULL,&count,&h);
  if(Plan.stage>=4&&s==EFI_NOT_FOUND)return EFI_SUCCESS;
  if(s!=EFI_SUCCESS||count!=1||!h){if(h)b->FreePool(h);return EFI_NOT_READY;}
  EFI_GRAPHICS_OUTPUT_PROTOCOL *g=NULL;s=Interface(h[0],&GopGuid2,(VOID **)&g);
  BOOLEAN ok=s==EFI_SUCCESS&&Path(h[0],DgopPath,sizeof(DgopPath))&&g->Mode&&g->Mode->Info&&
    g->Mode->Info->HorizontalResolution==1920&&g->Mode->Info->VerticalResolution==1080&&
    g->Mode->FrameBufferBase==0xf000000000ULL&&g->Mode->FrameBufferSize==0x870000;
  if(h)b->FreePool(h);
  if(!ok||!first)return ok?EFI_SUCCESS:EFI_NOT_READY;
  Plan.gop_interface=g;Plan.mode=*g->Mode;Plan.info=*g->Mode->Info;
  return EFI_SUCCESS;
}
static EFI_STATUS Alive(VOID) {
  EFI_SYSTEM_TABLE *t=Plan.table;VOID *in=NULL,*out=NULL,*err=NULL;
  EFI_GUID InputGuid=EFI_SIMPLE_TEXT_INPUT_PROTOCOL_GUID,TextGuid2=EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL_GUID;
  if(t->ConsoleOutHandle!=Plan.console||t->ConsoleInHandle!=Plan.input||t->StandardErrorHandle!=Plan.error||
     t->ConIn!=Plan.keys||t->ConOut!=Plan.text||t->StdErr!=Plan.err||!t->ConIn||
     (UINTN)t->ConIn->Reset!=Plan.key_reset||(UINTN)t->ConIn->ReadKeyStroke!=Plan.key_read||
     t->ConIn->WaitForKey!=Plan.key_wait)return EFI_NOT_READY;
  EFI_STATUS s=Interface(Plan.input,&InputGuid,&in);
  if(s==EFI_SUCCESS)s=Interface(Plan.console,&TextGuid2,&out);
  if(s==EFI_SUCCESS)s=Interface(Plan.error,&TextGuid2,&err);
  return s==EFI_SUCCESS&&(in!=Plan.keys||out!=Plan.text||err!=Plan.err)?EFI_NOT_READY:s;
}
static EFI_STATUS ReadConfig(UINT32 c[64]) {
  EFI_PCI_IO_PROTOCOL *io=NULL;UINTN seg,bus,dev,fun;
  EFI_STATUS s=Interface(Plan.pci,&PciGuid,(VOID **)&io);
  if(s!=EFI_SUCCESS||io!=Gu.pci||!io->Pci.Read||!io->GetLocation)return EFI_NOT_READY;
  s=io->GetLocation(io,&seg,&bus,&dev,&fun);
  if(s!=EFI_SUCCESS||seg||bus!=1||dev||fun)return EFI_UNSUPPORTED;
  s=io->Pci.Read(io,EfiPciIoWidthUint32,0,64,c);
  if(s==EFI_SUCCESS&&(!GuTargetConfig(c,seg,bus,dev,fun)||(c[1]&4)))s=EFI_UNSUPPORTED;
  return s;
}
static BOOLEAN ConfigStable(const UINT32 c[64]) {
  for(UINTN i=0;i<64;i++) {
    UINT32 drift=c[i]^Plan.config[i];
    if(!drift)continue;
    BOOLEAN allowed=Gu.flrs==1&&i==0x80/4&&Plan.config[0x78/4]==0x0012b410&&
      !(c[i]&~Plan.config[i])&&!(drift&~0x00090000u);
    if(!allowed) {
      NOTE("PCI config drift refused: offset=%#x captured=%#x now=%#x bits=%#x flrs=%u\n",
        (unsigned)(i*4),Plan.config[i],c[i],drift,Gu.flrs);
      return FALSE;
    }
    NOTE("PCI status drift tolerated: offset=%#x captured=%#x now=%#x bits=%#x flrs=%u\n",
      (unsigned)(i*4),Plan.config[i],c[i],drift,Gu.flrs);
  }
  return TRUE;
}
static EFI_STATUS Bar(VOID) {
  EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *d=NULL;UINT64 support=0;
  if(!Gu.pci->GetBarAttributes)return EFI_UNSUPPORTED;
  EFI_STATUS s=Gu.pci->GetBarAttributes(Gu.pci,0,&support,(VOID **)&d);
  if(s==EFI_SUCCESS&&(!d||d->Desc!=0x8a||d->Len!=43||d->ResType||
     d->AddrRangeMin!=GU_BAR0_BASE||d->AddrRangeMax!=GU_BAR0_BASE+0xffffffULL||
     d->AddrTranslationOffset||d->AddrLen!=0x1000000))s=EFI_UNSUPPORTED;
  if(d&&Plan.table->BootServices->FreePool(d)!=EFI_SUCCESS)s=EFI_DEVICE_ERROR;
  return s;
}
/* Freeze the exact captured ownership graph of the card and its console. */
static EFI_STATUS Consumers(VOID) {
  UINTN pci_nv=0,pci_child=0,gop_text=0,gop_virtual=0,gop_gfx=0,gop_edid=0;
  for(UINTN i=0;i<Original.count;i++) {
    const EDGE *e=&Original.edges[i];
    if(e->handle==Plan.pci) {
      if(e->count!=1)return EFI_ACCESS_DENIED;
      if(RawGuid(e,PciIoGuid)&&e->attributes==EFI_OPEN_PROTOCOL_BY_DRIVER&&e->controller==Plan.pci){Plan.nv_agent=e->agent;pci_nv++;}
      else if(RawGuid(e,DevPathGuid)&&e->attributes==EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER&&e->controller==Plan.gop){pci_child++;}
      else return EFI_ACCESS_DENIED;
    } else if(e->handle==Plan.gop) {
      if(e->count!=1)return EFI_ACCESS_DENIED;
      if(RawGuid(e,TextOutGuid)&&e->attributes==EFI_OPEN_PROTOCOL_BY_DRIVER&&e->controller==Plan.gop){Plan.out_agent=e->agent;gop_text++;}
      else if(RawGuid(e,TextOutGuid)&&e->attributes==EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER&&e->controller==Plan.console){gop_virtual++;}
      else if(RawGuid(e,GopGuid)&&e->attributes==EFI_OPEN_PROTOCOL_BY_DRIVER&&e->controller==Plan.gop){Plan.gfx_agent=e->agent;gop_gfx++;}
      else if(RawGuid(e,EdidGuid)&&e->attributes==EFI_OPEN_PROTOCOL_BY_DRIVER&&e->controller==Plan.gop){Plan.edid_agent=e->agent;gop_edid++;}
      else return EFI_ACCESS_DENIED;
    }
  }
  if(pci_nv!=1||pci_child!=1||gop_text!=1||gop_virtual!=1||gop_gfx!=1||gop_edid!=1||
     !Plan.nv_agent||!Plan.out_agent||!Plan.gfx_agent||!Plan.edid_agent||
     Plan.nv_agent==Plan.out_agent||Plan.out_agent==Plan.gfx_agent||
     Plan.gfx_agent==Plan.edid_agent||Plan.edid_agent==Plan.nv_agent)return EFI_ACCESS_DENIED;
  for(UINTN i=0;i<4;i++){EFI_STATUS s=Driver(i);if(s!=EFI_SUCCESS)return s;}
  return EFI_SUCCESS;
}
EFI_STATUS GuAsusPreflight(EFI_SYSTEM_TABLE *t,EFI_HANDLE self,EFI_HANDLE pci) {
  if(Plan.mutated||Plan.ready)return EFI_ALREADY_STARTED;
  memset(&Plan,0,sizeof(Plan));Plan.table=t;Plan.self=self;Plan.pci=pci;
  if(!t||!t->BootServices||!t->RuntimeServices||!t->RuntimeServices->GetVariable||!self||!pci||
     Gu.handle!=pci||Gu.image!=self||Gu.bs!=t->BootServices||
     !Gu.profile_verified||!Gu.prom_verified||Gu.started||Gu.owned||Gu.armed)return EFI_INVALID_PARAMETER;
  EFI_BOOT_SERVICES *b=t->BootServices;
  if(!b->DisconnectController||!b->OpenProtocol||!b->CloseProtocol||!b->RaiseTPL||!b->RestoreTPL||
     !b->ProtocolsPerHandle||!b->OpenProtocolInformation||!b->LocateHandleBuffer||!b->HandleProtocol)return EFI_UNSUPPORTED;
  Plan.console=t->ConsoleOutHandle;Plan.input=t->ConsoleInHandle;Plan.error=t->StandardErrorHandle;
  Plan.text=t->ConOut;Plan.err=t->StdErr;Plan.keys=t->ConIn;
  if(!Plan.console||Plan.console!=Plan.input||Plan.console!=Plan.error||!Plan.text||!Plan.err||!Plan.keys||
     !Plan.keys->ReadKeyStroke||!Plan.keys->Reset||!Plan.keys->WaitForKey)return EFI_UNSUPPORTED;
  Plan.key_reset=(UINTN)Plan.keys->Reset;Plan.key_read=(UINTN)Plan.keys->ReadKeyStroke;Plan.key_wait=Plan.keys->WaitForKey;
  if(!Path(pci,TargetPath,sizeof(TargetPath))||!GuAd102ColdEntry())return EFI_NOT_READY;
  EFI_STATUS s=EFI_SUCCESS;
  EFI_HANDLE *h=NULL;UINTN count=0;BOOLEAN gop_found=FALSE;
  s=b->LocateHandleBuffer(ByProtocol,&PathGuid,NULL,&count,&h);
  for(UINTN i=0;s==EFI_SUCCESS&&i<count;i++)
    if(Path(h[i],DgopPath,sizeof(DgopPath))){if(gop_found||h[i]==Plan.pci)s=EFI_UNSUPPORTED;Plan.gop=h[i];gop_found=TRUE;}
  if(h)b->FreePool(h);
  if(s==EFI_SUCCESS&&!gop_found)s=EFI_NOT_READY;
  if(s==EFI_SUCCESS)s=ConsoleRoute(TRUE);
  if(s==EFI_SUCCESS)s=Bar();
  if(s==EFI_SUCCESS)s=ReadConfig(Plan.config);
  if(s==EFI_SUCCESS)s=Gu.pci->Attributes?Gu.pci->Attributes(Gu.pci,EfiPciIoAttributeOperationGet,0,&Plan.attributes):EFI_UNSUPPORTED;
  UINT64 decode=(Plan.config[1]&1?EFI_PCI_IO_ATTRIBUTE_IO:0)|(Plan.config[1]&2?EFI_PCI_IO_ATTRIBUTE_MEMORY:0);
  if(s==EFI_SUCCESS&&(Plan.attributes&(EFI_PCI_IO_ATTRIBUTE_IO|EFI_PCI_IO_ATTRIBUTE_MEMORY|EFI_PCI_IO_ATTRIBUTE_BUS_MASTER))!=decode)s=EFI_NOT_READY;
  if(s==EFI_SUCCESS)s=Snapshot(&Original);
  if(s==EFI_SUCCESS)s=Consumers();
  GuLog("AD102 dGPU preflight: status=%#llx graph=%zu out=%#llx gfx=%#llx edid=%#llx nv=%#llx\n",
    (unsigned long long)s,(size_t)Original.count,(unsigned long long)(UINTN)Plan.out_agent,
    (unsigned long long)(UINTN)Plan.gfx_agent,(unsigned long long)(UINTN)Plan.edid_agent,
    (unsigned long long)(UINTN)Plan.nv_agent);
  if(s==EFI_SUCCESS)Plan.ready=TRUE;return s;
}
static BOOLEAN DecodeMask(const UINT32 c[64],UINT64 *mask) {
  *mask=0;UINT32 missing=Plan.config[1]&~c[1]&3;
  if((Plan.config[1]&6)!=2||(c[1]&4))return FALSE;
  for(UINTN i=0;i<64;i++)if((c[i]|(i==1?missing:0))!=Plan.config[i])return FALSE;
  *mask=(missing&1?EFI_PCI_IO_ATTRIBUTE_IO:0)|(missing&2?EFI_PCI_IO_ATTRIBUTE_MEMORY:0);return TRUE;
}
static EFI_STATUS ClaimRestore(VOID) {
  EFI_BOOT_SERVICES *b=Plan.table->BootServices;EFI_PCI_IO_PROTOCOL *io=NULL;
  UINT32 c[64];UINT64 mask=0,attrs=0,supported=0;
  EFI_STATUS s=ReadConfig(c);if(s==EFI_SUCCESS&&!DecodeMask(c,&mask))s=EFI_NOT_READY;
  GuLog("AD102 decode restore intent: status=%#llx IO_MEM_mask=%#llx\n",(unsigned long long)s,(unsigned long long)mask);
  EFI_TPL old=b->RaiseTPL(TPL_NOTIFY);
  if(s==EFI_SUCCESS)s=ReadConfig(c);
  if(s==EFI_SUCCESS&&!DecodeMask(c,&mask))s=EFI_NOT_READY;
  if(s==EFI_SUCCESS)s=Snapshot(&Current);
  if(s==EFI_SUCCESS&&!Expected(4,FALSE))s=EFI_NOT_READY;
  if(s==EFI_SUCCESS)s=b->OpenProtocol(Plan.pci,&PciGuid,(VOID **)&io,Plan.self,NULL,EFI_OPEN_PROTOCOL_EXCLUSIVE);
  if(s==EFI_SUCCESS){Plan.claimed=TRUE;Gu.owned=TRUE;if(io!=Gu.pci)s=EFI_COMPROMISED_DATA;}
  if(s==EFI_SUCCESS)s=io->Attributes(io,EfiPciIoAttributeOperationGet,0,&attrs);
  if(s==EFI_SUCCESS&&attrs!=(Plan.attributes&~mask))s=EFI_NOT_READY;
  if(s==EFI_SUCCESS)s=io->Attributes(io,EfiPciIoAttributeOperationSupported,0,&supported);
  if(s==EFI_SUCCESS&&(supported&mask)!=mask)s=EFI_UNSUPPORTED;
  if(s==EFI_SUCCESS)s=Snapshot(&Current);
  if(s==EFI_SUCCESS&&!Expected(4,TRUE))s=EFI_NOT_READY;
  if(s==EFI_SUCCESS&&mask)s=io->Attributes(io,EfiPciIoAttributeOperationEnable,mask,NULL);
  b->RestoreTPL(old);
  if(s==EFI_SUCCESS)s=ReadConfig(c);
  if(s==EFI_SUCCESS&&!Equal(c,Plan.config,sizeof(c)))s=EFI_NOT_READY;
  if(s==EFI_SUCCESS)s=io->Attributes(io,EfiPciIoAttributeOperationGet,0,&attrs);
  if(s==EFI_SUCCESS&&attrs!=Plan.attributes)s=EFI_NOT_READY;
  if(s==EFI_SUCCESS)s=Bar();
  if(s==EFI_SUCCESS&&!GuAd102ColdEntry())s=EFI_NOT_READY;
  GuLog("AD102 exclusive claim/decode result: status=%#llx\n",(unsigned long long)s);return s;
}
EFI_STATUS GuAsusRelease(VOID) {
  if(!Plan.ready||Plan.mutated||Gu.started||Gu.owned)return EFI_NOT_READY;
  EFI_BOOT_SERVICES *b=Plan.table->BootServices;
  static const UINTN order[4]={0,1,2,3};          /* ConSplitter, GFX console, EDID, nvgop; stages 1-3 detach from the GOP handle, 4 from PCI */
  for(UINTN step=0;step<4;step++) {
    UINTN stage=step+1,which=order[step];
    GuLog("AD102 dGPU driver detach intent: stage=%zu agent=%zu\n",(size_t)stage,(size_t)which);
    EFI_STATUS s=Alive();UINT32 c[64];
    if(s==EFI_SUCCESS)s=ReadConfig(c);
    if(s==EFI_SUCCESS&&!Equal(c,Plan.config,sizeof(c)))s=EFI_NOT_READY;
    if(s==EFI_SUCCESS&&!GuAd102ColdEntry())s=EFI_NOT_READY;
    if(s==EFI_SUCCESS&&stage<4)s=ConsoleRoute(FALSE);
    EFI_TPL old=b->RaiseTPL(TPL_NOTIFY);
    if(s==EFI_SUCCESS)s=Snapshot(&Current);
    if(s==EFI_SUCCESS&&!Expected(stage-1,FALSE))s=EFI_NOT_READY;
    if(s==EFI_SUCCESS)s=Driver(which);
    b->RestoreTPL(old);
    if(s!=EFI_SUCCESS||Gu.fault)return s==EFI_SUCCESS?EFI_DEVICE_ERROR:s;
    /* Record before the callback: an error may follow a partial driver stop. */
    Plan.mutated=TRUE;
    s=b->DisconnectController(stage==4?Plan.pci:Plan.gop,
      stage==1?Plan.out_agent:stage==2?Plan.gfx_agent:stage==3?Plan.edid_agent:Plan.nv_agent,NULL);
    GuLog("AD102 dGPU driver detach return: stage=%zu status=%#llx\n",(size_t)stage,(unsigned long long)s);
    if(s!=EFI_SUCCESS||Gu.fault)return s==EFI_SUCCESS?EFI_DEVICE_ERROR:s;
    Plan.stage=stage;
    s=Snapshot(&Current);
    if(s==EFI_SUCCESS&&!Expected(stage,FALSE))s=EFI_NOT_READY;
    if(s==EFI_SUCCESS)s=Alive();
    if(s==EFI_SUCCESS&&stage<4)s=ConsoleRoute(FALSE);
    if(s!=EFI_SUCCESS)return s;
  }
  EFI_STATUS s=ClaimRestore();
  if(s==EFI_SUCCESS&&!GuAsusReleased())s=EFI_NOT_READY;
  return s;
}
BOOLEAN GuAsusReleased(VOID) {
  UINT32 c[64];UINT64 attrs=0;const char *failed=NULL;
  if(!Plan.ready||Plan.stage!=4||!Plan.claimed||!Gu.owned||Gu.fault)failed="state";
  else if(Alive()!=EFI_SUCCESS)failed="console";
  else if(ConsoleRoute(FALSE)!=EFI_SUCCESS)failed="dGPU-GOP-gone";
  else if(ReadConfig(c)!=EFI_SUCCESS)failed="config-read";
  else if(!ConfigStable(c))failed="config-drift";
  else if(Gu.pci->Attributes(Gu.pci,EfiPciIoAttributeOperationGet,0,&attrs)!=EFI_SUCCESS||attrs!=Plan.attributes)failed="attributes";
  else if(Snapshot(&Current)!=EFI_SUCCESS)failed="graph-snapshot";
  else if(!Expected(4,TRUE)){failed="graph";GraphDiff(TRUE);}
  if(failed)NOTE("AD102 dGPU release check failed: %s attributes=%#llx expected=%#llx graph=%zu original=%zu fault=%u flrs=%u\n",
    failed,(unsigned long long)attrs,(unsigned long long)Plan.attributes,(size_t)Current.count,(size_t)Original.count,Gu.fault,Gu.flrs);
  return !failed;
}
EFI_STATUS GuAsusFinish(VOID) {
  if(!GuSafeToReturn()||!Gu.quiesced||Gu.flrs!=1||!GuAsusReleased()){NOTE("AD102 dGPU finish refused before close\n");return EFI_NOT_READY;}
  EFI_STATUS s=Plan.table->BootServices->CloseProtocol(Plan.pci,&PciGuid,Plan.self,NULL);
  if(s!=EFI_SUCCESS){NOTE("AD102 dGPU finish close status=%#llx\n",(unsigned long long)s);return s;}
  Plan.claimed=FALSE;Gu.owned=FALSE;
  s=Snapshot(&Current);if(s==EFI_SUCCESS&&!Expected(4,FALSE)){s=EFI_NOT_READY;GraphDiff(FALSE);}
  if(s!=EFI_SUCCESS)NOTE("AD102 dGPU finish after close status=%#llx graph=%zu\n",(unsigned long long)s,(size_t)Current.count);
  return s;
}
BOOLEAN GuAsusMutated(VOID){return Plan.mutated;}
BOOLEAN GuAsusReadyForWindows(VOID) {
  if(!Plan.ready||Plan.stage!=4||Plan.claimed||Gu.owned||Gu.fault||!Gu.quiesced||Gu.flrs!=1||!GuSafeToReturn())return FALSE;
  UINT32 c[64];
  if(Alive()!=EFI_SUCCESS||ConsoleRoute(FALSE)!=EFI_SUCCESS||ReadConfig(c)!=EFI_SUCCESS||!ConfigStable(c)||
     Snapshot(&Current)!=EFI_SUCCESS||!Expected(4,FALSE))return FALSE;
  UINT32 chip=GuRead32(0),cpu=GuRead32(0x110100),hi=GuRead32(0x1fa828);
  UINT32 ss0=GuRead32(0x82381c),ss1=GuRead32(0x823820),readout=GuRead32(0x823818),plm=GuRead32(0x823804);
  return !Gu.fault&&chip==0x192000a1&&(cpu&0x10)&&cpu!=0xffffffff&&
    (cpu&0xffff0000)!=0xbadf0000&&!(hi>>4)&&ss0==0x88888888&&ss1==8&&
    !(readout&0x07ffffff)&&readout!=0xffffffff&&plm==0xffffffff;
}
