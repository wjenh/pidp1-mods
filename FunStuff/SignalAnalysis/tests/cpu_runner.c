/* Executes the installed pdp1.o in a separate process, with no panel, debugger,
 * networking, files/devices, or running-emulator state. CPU execution is real;
 * device integration and wall-clock pacing are explicitly not tested here. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdatomic.h>
#include <time.h>
#include "pdp1.h"
#include "configuration.h"

#ifndef RUNNER_CYCLE_LIMIT
#define RUNNER_CYCLE_LIMIT 2000000
#endif

extern bool lailiaEnabled, core1DEnabled, all1DEnabled;
_Atomic int ad1Work;
static unsigned tape[65536*2], tapeCount, tapeIndex;
static int tapeMode;
void ad1Service(PDP1 *p) { (void)p; }
void ad1ThrottleWait(PDP1 *p, uint64_t n) { (void)p; (void)n; }
uint64_t gettime(void) { return 0; }
ConfigurationP getConfiguration(void) { return NULL; }
ConfigurationSettingP findConfigurationSetting(ConfigurationP p, char *s)
{ (void)p; (void)s; return NULL; }
void updatelights(PDP1 *p, Panel *q) { (void)p; (void)q; }
void updatelights_pwm(Panel *p, int n) { (void)p; (void)n; }
void dynamicIotProcessorDoPoll(PDP1 *p) { (void)p; }
int dynamicIotProcessor(PDP1 *p, int dev, int pulse, int completion)
{
    (void)completion;
    if (dev == 002 && tapeMode) {
        if (pulse) {
            if (tapeIndex >= tapeCount) { fprintf(stderr,"Tape exhausted\n"); exit(4); }
            p->io=tape[tapeIndex++];
            p->ios=1;
        }
        return 1;
    }
    if (dev != 074 && dev != 054 && dev != 056) {
        fprintf(stderr, "Forbidden device IOT %02o in CPU-only test\n", dev);
        exit(4);
    }
    return 0; /* EEM/LEM, LSM, CBS are handled by the original CPU. */
}

static void load(PDP1 *p, const char *path)
{
    FILE *f = fopen(path,"r");
    unsigned a,w;
    if (!f) { perror(path); exit(2); }
    while (fscanf(f,"%o: %o",&a,&w)==2) {
        if (a >= MAXMEM || w > WORDMASK) { fprintf(stderr,"Bad image\n"); exit(2); }
        p->core[a]=w;
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    static PDP1 p;
    if (argc != 5 && argc != 6) { fprintf(stderr,"cpu_runner image.mem input.mem output.mem start_octal [loader.words]\n"); return 2; }
    load(&p,argv[1]);
    if (argc==6) {
        FILE *f=fopen(argv[5],"r");
        if (!f) { perror(argv[5]); return 2; }
        while (tapeCount<sizeof(tape)/sizeof(tape[0]) && fscanf(f,"%o",&tape[tapeCount])==1) tapeCount++;
        fclose(f); tapeMode=1;
    } else load(&p,argv[2]);
    /* Disable every PDP-1D extension: only the frozen original ISA is allowed. */
    lailiaEnabled=core1DEnabled=all1DEnabled=false;
    p.muldiv_sw=1;
    unsigned start=(unsigned)strtoul(argv[4],NULL,8);
    p.ta=tapeMode ? 07751 : (start&07777); p.eta=tapeMode ? 0 : (start&0170000); p.extend_sw=1;
#ifdef CPU_RUNNER_SETUP
    CPU_RUNNER_SETUP(&p);
#endif
    p.start_sw=1; spec(&p); p.start_sw=0;
    uint64_t cycles=0;
    int injected=!tapeMode;
    while (p.run && cycles<RUNNER_CYCLE_LIMIT) {
        if (!injected && !p.cyc && !p.bc && (p.epc|p.pc)==start) {
            load(&p,argv[2]); injected=1;
        }
        p.simtime += 5000; /* main.c advances the 5 us memory-cycle time. */
        cycle(&p); cycles++;
    }
    if (p.run) { fprintf(stderr,"Cycle budget exceeded at %06o\n",p.epc|p.pc); return 3; }
    if (tapeMode && (!injected || tapeIndex!=tapeCount)) {
        fprintf(stderr,"Loader failed: injected=%d words=%u/%u\n",injected,tapeIndex,tapeCount); return 3;
    }
    FILE *f=fopen(argv[3],"w");
    if (!f) { perror(argv[3]); return 2; }
    for(unsigned a=0;a<MAXMEM;a++) fprintf(f,"%06o: %06o\n",a,p.core[a]);
    fclose(f);
    printf("cycles=%llu simulated_ns=%llu pc=%06o overflow=%d extend=%d\n",
        (unsigned long long)cycles,(unsigned long long)p.simtime,p.epc|p.pc,p.ov1,p.exd);
    return 0;
}
