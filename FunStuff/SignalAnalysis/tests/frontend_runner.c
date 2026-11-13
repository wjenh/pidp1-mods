/* Original target CPU + deterministic read-only drum and completion/status adapters.
 * This validates CPU/device call sequencing, not the plugins' timing or drawing. */
#include <string.h>
#ifndef RUNNER_CYCLE_LIMIT
#define RUNNER_CYCLE_LIMIT 12000000
#endif
#define dynamicIotProcessor cpu_only_iot
#define dynamicIotProcessorDoPoll cpu_only_poll
#ifdef SAW_REAL_TAPE
#include "pdp1.h"
static PDP1 *tapeCpu;
static unsigned tapeMissInjected;
static void tape_setup(PDP1 *p);
#define CPU_RUNNER_SETUP(p) do { (p)->sbs16=getenv("SAW_TEST_SBS16")!=NULL; core1DEnabled=all1DEnabled=lailiaEnabled=true; tape_setup(p); } while (0)

#else
#define CPU_RUNNER_SETUP(p) do { (p)->sbs16=getenv("SAW_TEST_SBS16")!=NULL; } while (0)
#endif
#define main cpu_only_main
#include "cpu_runner.c"
#undef main
#undef dynamicIotProcessor
#undef dynamicIotProcessorDoPoll
#ifdef SAW_REAL_TAPE
#include "microtape.h"
extern void dynamicReq(PDP1 *,int);
void initiateBreak(int chan) { dynamicReq(tapeCpu,chan); }
static void tape_setup(PDP1 *p) {
    tapeCpu=p;mtStart();
    const char *path=getenv("SAW_TEST_TAPE_IMAGE");char err[512];
    if (path && mt555MountFile(mt550Unit(mtControl(),8),path,true,false,err,sizeof(err))) {
        mt550UnitRemounted(mtControl(),8,0);
        if (getenv("SAW_START_TAPE_BLOCK")) mt555Park(mt550Unit(mtControl(),8),MT_BA+(int64_t)strtoul(getenv("SAW_START_TAPE_BLOCK"),NULL,10)*MT_BLOCK_NS);
    }
}
#endif

static Word disk[32*4096];
static unsigned diaword,dwcword,dstaddr,readCount,readField,readOffset;
static unsigned drumReads,displayStarts,lastDisplay;
static uint64_t drumDue,displayDue;
static int drumPending,displayStopped,drumError;
static Word displaySnapshot[1984];
static unsigned snapshotAddr;
static uint64_t afterDone,firstDone;
static unsigned senseMode, gainMode;
static int clockEnabled;
static uint64_t displayDelay=5000000, clockBase;
static unsigned clockOffset;
static int clockRead;
extern void dynamicReq(PDP1 *,int);
static FILE *frameTrace, *listTrace;
static unsigned tracedList;
static unsigned frameCounterAddr, tracedFrames;


void dynamicIotProcessorDoPoll(PDP1 *p)
{
#ifdef SAW_REAL_TAPE
    const char *stageAddress=getenv("SAW_TAPE_STAGE_ADDR");
    const char *inject=getenv("SAW_INJECT_TAPE_MISS");
    const char *triesAddress=getenv("SAW_TAPE_TRIES_ADDR");
    unsigned attempt=(inject && !strcmp(inject,"repeat") && triesAddress)?p->core[strtoul(triesAddress,NULL,8)]:0;
    unsigned bit=1u<<(attempt&7);
    if (!(tapeMissInjected&bit) && inject && stageAddress &&
        p->core[strtoul(stageAddress,NULL,8)]==6 && mtControl()->mode==MT_MODE_READ && mtControl()->df) {
        p->simtime+=1000000;tapeMissInjected|=bit;
    }
    mtIOPoll(p);
    if (p->core[0102] && getenv("SAW_STOP_ON_FAULT")) p->run=0;
#endif
    if (frameTrace && p->core[frameCounterAddr]!=tracedFrames) {
        tracedFrames=p->core[frameCounterAddr];
        fprintf(frameTrace,"%u",tracedFrames);
        for (unsigned k=0;k<129;k++) {
            uint64_t power=(uint64_t)p->core[012000+k]*131072+p->core[012400+k];
            fprintf(frameTrace," %llu",(unsigned long long)power);
        }
        fputc('\n',frameTrace);
    }
    if (senseMode==1) p->ss=040;  // switch 1: continuous
    if (senseMode==2 && firstDone) {  // one rising-edge pulse on switch 2
        uint64_t elapsed=p->simtime-firstDone;
        p->ss=(elapsed>=100000000 && elapsed<1200000000)?020:0;
    }
    if (gainMode==1) p->ss|=010;  // S3 high gain from startup
    if (gainMode==2 && firstDone) {  // change gain in hold mode, then return to normal
        uint64_t elapsed=p->simtime-firstDone;
        p->ss=(p->ss&~010)|((elapsed>=100000000 && elapsed<1200000000)?010:0);
    }
    if (afterDone && p->core[0100]==1) {
        if (!firstDone) firstDone=p->simtime;
        if (p->simtime-firstDone>=afterDone) p->run=0;
    }
    if (drumPending && p->simtime>=drumDue) {
        if (!drumError) for (unsigned n=0;n<readCount;n++)
            p->core[(dstaddr&0170000)|((dstaddr+n)&07777)] = disk[readField*4096+((readOffset+n)&07777)];
        p->cksflags &= ~1;
        drumPending=0;
        dynamicReq(p,0);       // completion may share the display break channel
    }
    if (displayDue && p->simtime>=displayDue) {
        if (snapshotAddr && memcmp(displaySnapshot,p->core+snapshotAddr,sizeof(displaySnapshot))) {
            fprintf(stderr,"CPU modified an active display list\n");exit(4);
        }
        displayStopped=1;displayDue=0;
        if (snapshotAddr) dynamicReq(p,0);
    }
}
int dynamicIotProcessor(PDP1 *p,int dev,int pulse,int completion)
{
    unsigned cmd=(p->mb>>6)&077;
#ifdef SAW_REAL_TAPE
    if (dev==001 && mtOwnsIot(p->mb)) {
        if (pulse && (cmd==2 || cmd==6 || (cmd==4 && ((p->io&7)==3 || (p->io&7)==7)))) {
            fprintf(stderr,"Forbidden tape write/erase operation\n");exit(4);
        }
        if (pulse && cmd==2 && ((p->io>>12)&017)!=8) {
            fprintf(stderr,"Test must mount only drive 8\n");exit(4);
        }
        return mtIotHandler(p,pulse,completion);
    }
#endif
    if (dev==061 || dev==062 || dev==063) {
        if (pulse) return 1;
        if (dev==061) {
            if (cmd) { fprintf(stderr,"Unexpected DBA\n");exit(4); }
            diaword=p->io;
        } else if (dev==062) {
            if (cmd==020) { p->io=drumError?0400000:0;return 1; }
            dwcword=p->io;
        } else {
            if (cmd || drumPending || !(diaword&0400000) || (dwcword&0400000)) {
                fprintf(stderr,"Invalid/overlapping/write drum transfer\n");exit(4);
            }
            dstaddr=p->io&0177777;readField=(diaword>>12)&037;readOffset=diaword&07777;
            readCount=dwcword&07777;if (!readCount)readCount=4096;
            if (readCount!=128) { fprintf(stderr,"Unbounded drum DMA chunk: %u\n",readCount);exit(4); }
            drumPending=1;drumDue=p->simtime+2000000;drumReads++;p->cksflags|=1;
        }
        return 1;
    }
    if (dev==015 || dev==016 || dev==017) {
        if (!pulse) return 1;
        if (dev==015) {
            if (cmd==0) {
                lastDisplay=p->io&0177777;displayStarts++;displayStopped=0;displayDue=p->simtime+displayDelay;
                snapshotAddr=(lastDisplay>=020000 && lastDisplay<030000)?lastDisplay:0;
                if (snapshotAddr) memcpy(displaySnapshot,p->core+snapshotAddr,sizeof(displaySnapshot));
                if (listTrace && snapshotAddr && snapshotAddr!=tracedList) {
                    fprintf(listTrace,"%o",snapshotAddr);
                    for (unsigned n=0;n<1984;n++) fprintf(listTrace," %o",p->core[snapshotAddr+n]);
                    fputc('\n',listTrace);tracedList=snapshotAddr;
                }
            }
            else if (cmd==2) displayStopped=0;
            else { fprintf(stderr,"Unexpected 340 command\n");exit(4); }
        } else if (dev==017 && (cmd&2) && displayStopped) p->pc=(p->pc+1)&07777;
        return 1;
    }
    if (dev==032) {
        if (!pulse && cmd==020) clockEnabled=(p->ac&02000)!=0;
        if (!pulse && cmd==0) {
            if (!clockRead) { clockBase=p->simtime;clockRead=1; }
            p->io=clockEnabled?((p->simtime-clockBase)/1000000+clockOffset)%60000:0;
        }
        return 1;
    }
    if (dev==051 || dev==053 || dev==055) return 0;
    if (dev==033) return 0; /* CPU CKS includes our adapter's busy bit. */
    return cpu_only_iot(p,dev,pulse,completion);
}
int main(int argc,char **argv)
{
    if (argc!=6) { fprintf(stderr,"frontend_runner image.mem input.mem output.mem start_octal drum.drm\n");return 2; }
    FILE *f=fopen(argv[5],"rb");
    if (!f) { perror(argv[5]);return 2; }
    if (fread(disk,sizeof(Word),32*4096,f)!=32*4096) { fprintf(stderr,"Bad test drum\n");return 2; }
    fclose(f);
    drumError=getenv("SAW_TEST_DMA_ERROR")!=NULL;
    if (getenv("SAW_DISPLAY_DELAY_NS")) displayDelay=strtoull(getenv("SAW_DISPLAY_DELAY_NS"),NULL,10);
    if (getenv("SAW_CLOCK_OFFSET_MS")) clockOffset=(unsigned)strtoul(getenv("SAW_CLOCK_OFFSET_MS"),NULL,10);
    if (getenv("SAW_STOP_DELAY_NS")) afterDone=strtoull(getenv("SAW_STOP_DELAY_NS"),NULL,10);
    if (getenv("SAW_SENSE_MODE")) senseMode=(unsigned)strtoul(getenv("SAW_SENSE_MODE"),NULL,10);
    if (getenv("SAW_GAIN_MODE")) gainMode=(unsigned)strtoul(getenv("SAW_GAIN_MODE"),NULL,10);
    if (getenv("SAW_FRAME_TRACE")) {
        if (!getenv("SAW_FRAME_COUNTER_ADDR")) return 2;
        frameCounterAddr=(unsigned)strtoul(getenv("SAW_FRAME_COUNTER_ADDR"),NULL,8);
        frameTrace=fopen(getenv("SAW_FRAME_TRACE"),"w");
        if (!frameTrace) return 2;
    }
    if (getenv("SAW_LIST_TRACE")) { listTrace=fopen(getenv("SAW_LIST_TRACE"),"w");if (!listTrace) return 2; }
    int result=cpu_only_main(5,argv);
    if (listTrace) fclose(listTrace);
    if (frameTrace) fclose(frameTrace);
    printf("drum_reads=%u display_starts=%u last_display=%06o\n",drumReads,displayStarts,lastDisplay);
    return result;
}
