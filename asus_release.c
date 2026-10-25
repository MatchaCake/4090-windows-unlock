/* SPDX-License-Identifier: GPL-2.0-only */
/* ASUS X670E-I / Zotac AD102 ownership adapter (iGPU-console topology).
 * Rewritten from the MSI Z490/AD104 adapter, which itself adapted
 * impacifist/Windows-GPU-UNLOCK 9352abd snapshot/delta strategy.
 * CAP0003/CAP0004 show this board's 4090 PCI handle carries exactly ONE
 * relationship: the NVIDIA GOP driver binding (nvgop-ad1xx, BY_DRIVER PCI_IO).
 * There is no GOP child, no text-out and no graphics-console edge on the GPU;
 * the UEFI console lives on the iGOP at bus 105 for the whole transaction.
 * Release therefore reduces to: disconnect that one driver, then take the
 * exclusive PCI claim and restore IO/MEM decode. No driver code/data patch,
 * no firmware variables, no controller reconnect, no reset fallback. */
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
  EFI_SYSTEM_TABLE *table;EFI_HANDLE self,pci,nv_agent;
  EFI_HANDLE console,input,error;
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *text,*err;
  EFI_SIMPLE_TEXT_INPUT_PROTOCOL *keys;
  UINTN key_reset,key_read;EFI_EVENT key_wait;
  UINT32 config[64];UINT64 attributes;
  UINTN stage;BOOLEAN ready,mutated,claimed;
} PLAN;
static PLAN Plan;
static GRAPH Original,Current;
static EFI_GUID PciGuid=EFI_PCI_IO_PROTOCOL_GUID,PathGuid=EFI_DEVICE_PATH_PROTOCOL_GUID;
static EFI_GUID GopGuid=EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID,TextGuid=EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL_GUID;
static EFI_GUID InputGuid=EFI_SIMPLE_TEXT_INPUT_PROTOCOL_GUID;
static EFI_GUID GlobalGuid={0x8be4df61,0x93ca,0x11d2,{0xaa,0x0d,0,0xe0,0x98,0x03,0x2b,0x8c}};
static EFI_GUID BindingGuid=EFI_DRIVER_BINDING_PROTOCOL_GUID,ImageGuid=EFI_LOADED_IMAGE_PROTOCOL_GUID;
/* 4090: PNP0A03 / PCI(F1,D1) / PCI(F0,D0) — CAP0003 target path, bus1/dev0/fun0. */
static const UINT8 TargetPath[]={2,1,12,0,0xd0,0x41,3,10,0,0,0,0,1,1,6,0,1,1,1,1,6,0,0,0,127,255,4,0};
/* iGPU GOP/ConOut route: PNP0A03 / PCI(F1,D8) / PCI(F0,D0) / ADR(0x80010302). */
static const UINT8 IgopPath[]={2,1,12,0,0xd0,0x41,3,10,0,0,0,0,1,1,6,0,1,8,1,1,6,0,0,0,
  2,3,8,0,0x02,0x03,0x01,0x80,127,255,4,0};
/* NVIDIA GOP driver file-path node (identical node bytes to the AD104 board). */
static const UINT8 NvFile[]={4,4,28,0,'n',0,'v',0,'g',0,'o',0,'p',0,'-',0,'a',0,'d',0,'1',0,'x',0,'x',0,0,0,127,255,4,0};

static BOOLEAN Equal(const VOID *a,const VOID *b,UINTN n){return !memcmp(a,b,n);}
static BOOLEAN Guid(const EFI_GUID *a,const EFI_GUID *b){return Equal(a,b,16);}
static EFI_STATUS Interface(EFI_HANDLE h,EFI_GUID *g,VOID **p) {
  *p=NULL;EFI_STATUS s=Plan.table->BootServices->HandleProtocol(h,g,p);
  return s==EFI_SUCCESS&&!*p?EFI_COMPROMISED_DATA:s;
}
/* Match exact, bounded, single-instance paths; never accept a prefix alone. */
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
/* TRUE only when h's path is strictly longer than the GPU's own path and
 * starts with it: an unexpected child of the 4090 (capture shows none). */
static BOOLEAN GpuChild(EFI_HANDLE h) {
  EFI_DEVICE_PATH_PROTOCOL *p=NULL;
  if(h==Plan.pci||Interface(h,&PathGuid,(VOID **)&p)!=EFI_SUCCESS||!p)return FALSE;
  UINTN prefix=sizeof(TargetPath)-4,at=0;
  while(at<prefix) {
    UINTN n=p->Length[0]+((UINTN)p->Length[1]<<8);
    if(n<4||at+n>prefix)return FALSE;
    if(!Equal(p,(const UINT8 *)TargetPath+at,n))return FALSE;
    at+=n;p=(VOID *)((UINT8 *)p+n);
  }
  /* At the prefix boundary: a strict extension continues with a node longer than END. */
  return p->Length[0]+((UINTN)p->Length[1]<<8)>4;
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
    a->attributes==b->attributes&&a->count==b->count&&Guid(&a->protocol,&b->protocol);
}
static BOOLEAN Removed(const EDGE *e,UINTN stage) {
  return stage>=1&&e->handle==Plan.pci&&e->agent==Plan.nv_agent&&
    Guid(&e->protocol,&PciGuid);
}
/* Every endpoint that must not change relationships during the transaction. */
static BOOLEAN ProtectedHandle(EFI_HANDLE h) {
  return h&&(h==Plan.pci||h==Plan.self||h==Plan.nv_agent||
    h==Plan.console||h==Plan.input||h==Plan.error);
}
static BOOLEAN ProtectedEdge(const EDGE *e) {
  return ProtectedHandle(e->handle)||ProtectedHandle(e->agent)||ProtectedHandle(e->controller);
}
static BOOLEAN OwnEdge(const EDGE *e) {
  return e->handle==Plan.pci&&e->agent==Plan.self&&!e->controller&&
    Guid(&e->protocol,&PciGuid)&&e->attributes==EFI_OPEN_PROTOCOL_EXCLUSIVE&&e->count==1;
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
    /* New unrelated relationships (e.g. USB enumeration) are tolerated; any new
     * or removed relationship touching a protected endpoint is refused. */
    if(!KnownEdge(e,stage)&&ProtectedEdge(e))return FALSE;
    for(UINTN k=j+1;k<Current.count;k++)if(SameEdge(e,&Current.edges[k]))return FALSE;
  }
  return own==(claimed?1u:0u);
}
#define NOTE(...) do{if(GuBootLogOpen())GuLog(__VA_ARGS__);}while(0)
static VOID NoteEdge(const char *kind,const EDGE *e) {
  NOTE("AD102 graph %s: handle=%#llx agent=%#llx controller=%#llx protocol=%#x attributes=%#x count=%u\n",kind,
    (unsigned long long)(UINTN)e->handle,(unsigned long long)(UINTN)e->agent,(unsigned long long)(UINTN)e->controller,
    e->protocol.Data1,e->attributes,e->count);
}
static VOID GraphDiff(BOOLEAN claimed) {
  for(UINTN i=0;i<Original.count;i++) {
    if(Removed(&Original.edges[i],1))continue;
    UINTN matches=0;
    for(UINTN j=0;j<Current.count;j++)if(SameEdge(&Original.edges[i],&Current.edges[j]))matches++;
    if(matches!=1){NoteEdge(matches?"duplicate":"missing",&Original.edges[i]);return;}
  }
  for(UINTN j=0;j<Current.count;j++) {
    const EDGE *e=&Current.edges[j];
    if(!KnownEdge(e,1)&&!(claimed&&OwnEdge(e))&&ProtectedEdge(e)){NoteEdge("unexpected",e);return;}
    for(UINTN k=j+1;k<Current.count;k++)if(SameEdge(e,&Current.edges[k])){NoteEdge("duplicate",e);return;}
  }
}
/* The one driver this port is allowed to disconnect: nvgop-ad1xx as captured. */
static EFI_STATUS Driver(VOID) {
  EFI_DRIVER_BINDING_PROTOCOL *d=NULL;EFI_LOADED_IMAGE_PROTOCOL *im=NULL;
  EFI_STATUS s=Interface(Plan.nv_agent,&BindingGuid,(VOID **)&d);
  if(s!=EFI_SUCCESS||!d->Stop||d->DriverBindingHandle!=Plan.nv_agent||!d->ImageHandle||
     d->ImageHandle!=Plan.nv_agent||d->Version!=0x00070009u)return EFI_UNSUPPORTED;
  s=Interface(d->ImageHandle,&ImageGuid,(VOID **)&im);if(s!=EFI_SUCCESS)return s;
  if(!im->ImageBase||im->ImageSize!=161696||im->ImageCodeType!=EfiBootServicesCode||
     im->DeviceHandle!=Plan.pci||(UINTN)d->Stop!=(UINTN)im->ImageBase+0x6ec||!im->FilePath||
     !Equal(im->FilePath,NvFile,sizeof(NvFile)))return EFI_UNSUPPORTED;
  /* CAP0003 found no relocations in this code range; hash loaded code only. */
  return GuHashMatches((UINT8 *)im->ImageBase+0x280,137439,
    "1905d2b2d7e2022a36d1aba5e3f26faa085ea9966c0ce15cb6c1e10cf72d6f71")?EFI_SUCCESS:EFI_CRC_ERROR;
}
/* Console must stay on the captured iGOP route for the whole transaction. */
static EFI_STATUS ConsoleRoute(VOID) {
  EFI_BOOT_SERVICES *b=Plan.table->BootServices;
  UINT8 buffer[128];UINTN size=sizeof(buffer);UINT32 attributes=0;
  EFI_STATUS s=Plan.table->RuntimeServices->GetVariable(L"ConOut",&GlobalGuid,&attributes,&size,buffer);
  if(s!=EFI_SUCCESS||size!=sizeof(IgopPath)||memcmp(buffer,IgopPath,size))return EFI_NOT_READY;
  EFI_HANDLE *h=NULL;UINTN count=0;
  s=b->LocateHandleBuffer(ByProtocol,&GopGuid,NULL,&count,&h);
  if(s!=EFI_SUCCESS||count!=1||!h){if(h)b->FreePool(h);return EFI_NOT_READY;}
  EFI_GRAPHICS_OUTPUT_PROTOCOL *g=NULL;s=Interface(h[0],&GopGuid,(VOID **)&g);
  BOOLEAN ok=s==EFI_SUCCESS&&Path(h[0],IgopPath,sizeof(IgopPath))&&g->Mode&&g->Mode->Info&&
    g->Mode->Info->HorizontalResolution==1920&&g->Mode->Info->VerticalResolution==1080&&
    g->Mode->FrameBufferBase==0xf870000000ULL&&g->Mode->FrameBufferSize==0x7e9000;
  if(h)b->FreePool(h);
  return ok?EFI_SUCCESS:EFI_NOT_READY;
}
static EFI_STATUS Alive(VOID) {
  EFI_SYSTEM_TABLE *t=Plan.table;VOID *in=NULL,*out=NULL,*err=NULL;
  if(t->ConsoleOutHandle!=Plan.console||t->ConsoleInHandle!=Plan.input||t->StandardErrorHandle!=Plan.error||
     t->ConIn!=Plan.keys||t->ConOut!=Plan.text||t->StdErr!=Plan.err||!t->ConIn||
     (UINTN)t->ConIn->Reset!=Plan.key_reset||(UINTN)t->ConIn->ReadKeyStroke!=Plan.key_read||
     t->ConIn->WaitForKey!=Plan.key_wait)return EFI_NOT_READY;
  EFI_STATUS s=Interface(Plan.input,&InputGuid,&in);
  if(s==EFI_SUCCESS)s=Interface(Plan.console,&TextGuid,&out);
  if(s==EFI_SUCCESS)s=Interface(Plan.error,&TextGuid,&err);
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
/* Only preexisting CED/URD status bits may clear after FLR (upstream rule;
 * the captured control word at 0x78 matches the reference board's value). */
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
/* The 4090 handle must carry exactly the one captured NVIDIA binding. */
static EFI_STATUS Consumers(VOID) {
  UINTN pci=0;
  for(UINTN i=0;i<Original.count;i++) {
    const EDGE *e=&Original.edges[i];
    if(e->handle!=Plan.pci)continue;
    if(e->count!=1)return EFI_ACCESS_DENIED;
    if(Guid(&e->protocol,&PciGuid)&&e->attributes==EFI_OPEN_PROTOCOL_BY_DRIVER&&
       e->controller==Plan.pci){Plan.nv_agent=e->agent;pci++;}
    else return EFI_ACCESS_DENIED;
  }
  if(pci!=1||!Plan.nv_agent||Plan.nv_agent==Plan.self)return EFI_ACCESS_DENIED;
  return Driver();
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
  EFI_STATUS s=ConsoleRoute();
  if(s==EFI_SUCCESS)s=Alive();
  if(s==EFI_SUCCESS)s=Bar();
  if(s==EFI_SUCCESS)s=ReadConfig(Plan.config);
  if(s==EFI_SUCCESS)s=Gu.pci->Attributes?Gu.pci->Attributes(Gu.pci,EfiPciIoAttributeOperationGet,0,&Plan.attributes):EFI_UNSUPPORTED;
  UINT64 decode=(Plan.config[1]&1?EFI_PCI_IO_ATTRIBUTE_IO:0)|(Plan.config[1]&2?EFI_PCI_IO_ATTRIBUTE_MEMORY:0);
  if(s==EFI_SUCCESS&&(Plan.attributes&(EFI_PCI_IO_ATTRIBUTE_IO|EFI_PCI_IO_ATTRIBUTE_MEMORY|EFI_PCI_IO_ATTRIBUTE_BUS_MASTER))!=decode)s=EFI_NOT_READY;
  if(s==EFI_SUCCESS)s=Snapshot(&Original);
  if(s==EFI_SUCCESS)s=Consumers();
  if(s==EFI_SUCCESS) {
    EFI_HANDLE *all=NULL;UINTN count=0;
    s=b->LocateHandleBuffer(AllHandles,NULL,NULL,&count,&all);
    for(UINTN i=0;s==EFI_SUCCESS&&i<count;i++)
      if(GpuChild(all[i])){NOTE("AD102 preflight: unexpected GPU child handle=%#llx\n",(unsigned long long)(UINTN)all[i]);s=EFI_UNSUPPORTED;}
    if(all)b->FreePool(all);
  }
  GuLog("AD102 preflight: status=%#llx graph=%zu nv_agent=%#llx code_profile=1\n",
    (unsigned long long)s,(size_t)Original.count,(unsigned long long)(UINTN)Plan.nv_agent);
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
  if(s==EFI_SUCCESS&&!Expected(1,FALSE))s=EFI_NOT_READY;
  if(s==EFI_SUCCESS)s=b->OpenProtocol(Plan.pci,&PciGuid,(VOID **)&io,Plan.self,NULL,EFI_OPEN_PROTOCOL_EXCLUSIVE);
  if(s==EFI_SUCCESS){Plan.claimed=TRUE;Gu.owned=TRUE;if(io!=Gu.pci)s=EFI_COMPROMISED_DATA;}
  if(s==EFI_SUCCESS)s=io->Attributes(io,EfiPciIoAttributeOperationGet,0,&attrs);
  if(s==EFI_SUCCESS&&attrs!=(Plan.attributes&~mask))s=EFI_NOT_READY;
  if(s==EFI_SUCCESS)s=io->Attributes(io,EfiPciIoAttributeOperationSupported,0,&supported);
  if(s==EFI_SUCCESS&&(supported&mask)!=mask)s=EFI_UNSUPPORTED;
  if(s==EFI_SUCCESS)s=Snapshot(&Current);
  if(s==EFI_SUCCESS&&!Expected(1,TRUE))s=EFI_NOT_READY;
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
  GuLog("AD102 driver detach intent: nvgop-ad1xx binding on the 4090\n");
  EFI_STATUS s=Alive();UINT32 c[64];
  if(s==EFI_SUCCESS)s=ReadConfig(c);
  if(s==EFI_SUCCESS&&!Equal(c,Plan.config,sizeof(c)))s=EFI_NOT_READY;
  if(s==EFI_SUCCESS&&!GuAd102ColdEntry())s=EFI_NOT_READY;
  if(s==EFI_SUCCESS)s=ConsoleRoute();
  EFI_TPL old=b->RaiseTPL(TPL_NOTIFY);
  if(s==EFI_SUCCESS)s=Snapshot(&Current);
  if(s==EFI_SUCCESS&&!Expected(0,FALSE))s=EFI_NOT_READY;
  if(s==EFI_SUCCESS)s=Driver();
  b->RestoreTPL(old);
  if(s!=EFI_SUCCESS||Gu.fault)return s==EFI_SUCCESS?EFI_DEVICE_ERROR:s;
  /* Record before the callback: an error may follow a partial driver stop. */
  Plan.mutated=TRUE;
  s=b->DisconnectController(Plan.pci,Plan.nv_agent,NULL);
  GuLog("AD102 driver detach return: status=%#llx\n",(unsigned long long)s);
  if(s!=EFI_SUCCESS||Gu.fault)return s==EFI_SUCCESS?EFI_DEVICE_ERROR:s;
  Plan.stage=1;
  s=Snapshot(&Current);
  if(s==EFI_SUCCESS&&!Expected(1,FALSE))s=EFI_NOT_READY;
  if(s==EFI_SUCCESS)s=Alive();
  if(s==EFI_SUCCESS)s=ConsoleRoute();
  if(s!=EFI_SUCCESS)return s;
  s=ClaimRestore();
  if(s==EFI_SUCCESS&&!GuAsusReleased())s=EFI_NOT_READY;
  return s;
}
BOOLEAN GuAsusReleased(VOID) {
  UINT32 c[64];UINT64 attrs=0;const char *failed=NULL;
  if(!Plan.ready||Plan.stage!=1||!Plan.claimed||!Gu.owned||Gu.fault)failed="state";
  else if(Alive()!=EFI_SUCCESS)failed="console";
  else if(ConsoleRoute()!=EFI_SUCCESS)failed="iGOP-route";
  else if(ReadConfig(c)!=EFI_SUCCESS)failed="config-read";
  else if(!ConfigStable(c))failed="config-drift";
  else if(Gu.pci->Attributes(Gu.pci,EfiPciIoAttributeOperationGet,0,&attrs)!=EFI_SUCCESS||attrs!=Plan.attributes)failed="attributes";
  else if(Snapshot(&Current)!=EFI_SUCCESS)failed="graph-snapshot";
  else if(!Expected(1,TRUE)){failed="graph";GraphDiff(TRUE);}
  if(failed)NOTE("AD102 release check failed: %s attributes=%#llx expected=%#llx graph=%zu original=%zu fault=%u flrs=%u\n",
    failed,(unsigned long long)attrs,(unsigned long long)Plan.attributes,(size_t)Current.count,(size_t)Original.count,Gu.fault,Gu.flrs);
  return !failed;
}
EFI_STATUS GuAsusFinish(VOID) {
  if(!GuSafeToReturn()||!Gu.quiesced||Gu.flrs!=1||!GuAsusReleased()){NOTE("AD102 finish refused before close\n");return EFI_NOT_READY;}
  EFI_STATUS s=Plan.table->BootServices->CloseProtocol(Plan.pci,&PciGuid,Plan.self,NULL);
  if(s!=EFI_SUCCESS){NOTE("AD102 finish close status=%#llx\n",(unsigned long long)s);return s;}
  Plan.claimed=FALSE;Gu.owned=FALSE;
  s=Snapshot(&Current);if(s==EFI_SUCCESS&&!Expected(1,FALSE)){s=EFI_NOT_READY;GraphDiff(FALSE);}
  if(s!=EFI_SUCCESS)NOTE("AD102 finish after close status=%#llx graph=%zu\n",(unsigned long long)s,(size_t)Current.count);
  return s;
}
BOOLEAN GuAsusMutated(VOID){return Plan.mutated;}
BOOLEAN GuAsusReadyForWindows(VOID) {
  if(!Plan.ready||Plan.stage!=1||Plan.claimed||Gu.owned||Gu.fault||!Gu.quiesced||Gu.flrs!=1||!GuSafeToReturn())return FALSE;
  UINT32 c[64];
  if(Alive()!=EFI_SUCCESS||ConsoleRoute()!=EFI_SUCCESS||ReadConfig(c)!=EFI_SUCCESS||!ConfigStable(c)||
     Snapshot(&Current)!=EFI_SUCCESS||!Expected(1,FALSE))return FALSE;
  /* Fresh status after USB Close and before StartImage; no log callbacks. */
  UINT32 chip=GuRead32(0),cpu=GuRead32(0x110100),hi=GuRead32(0x1fa828);
  UINT32 ss0=GuRead32(0x82381c),ss1=GuRead32(0x823820),readout=GuRead32(0x823818),plm=GuRead32(0x823804);
  return !Gu.fault&&chip==0x192000a1&&(cpu&0x10)&&cpu!=0xffffffff&&
    (cpu&0xffff0000)!=0xbadf0000&&!(hi>>4)&&ss0==0x88888888&&ss1==8&&
    !(readout&0x07ffffff)&&readout!=0xffffffff&&plm==0xffffffff;
}
