#include <stdbool.h>

typedef u32 Word;
typedef u16 Addr;
#define WORDMASK 0777777
#define ADDRMASK 07777
#define EXTMASK 0170000
//#define MAXMEM (4*1024)
#define MAXMEM (64*1024)
#define NIL (void *)0

typedef struct PDP1 PDP1;
typedef struct DispCon DispCon;
typedef struct Panel Panel;

struct DispCon
{
	int fd;
	u64 last;
	u32 cmdbuf[128];
	u32 ncmds;
	u32 agetime;
};

struct PDP1
{
	int timernd;
	Panel *panel;

	Word ac;
	Word io;
	Word mb;
	Word ma;
	Word pc;
	Word ir;
	Word core[MAXMEM];

	Word ta;
	Word tw;

	bool start_sw;
	bool sbm_start_sw;
	bool stop_sw;
	bool continue_sw;
	bool examine_sw;
	bool deposit_sw;
	bool readin_sw;

	bool power_sw;
	bool single_cyc_sw;
	bool single_inst_sw;

	int run, run_enable;
	int cyc;
	int df1, df2;
	int bc;
	int ov1, ov2;
	int rim;
	int sbm;
	int ioc;
	int ihs;
	int ios;
	int ioh;
        int hsc;        // wje - add hsc light control, drum uses it

	// extensions
	int lai, lia;

	int ss;
	int pf;

	int r, rs, w, i;

	// seq break
	int sbs16;	// 16 channel, type 20
	// one bit per channel if type 20
	u16 req;	// highest prio channel
	u16 b1;		// on (only type 20)
	u16 b2;		// req
	u16 b3;		// req synchronized
	u16 b4;		// break held

	// type 10, multiply-divide
	int muldiv_sw;
	int scr;
	int smb, srm;

	// type 15, memory extension
	int extend_sw;
	int emc;
	int exd;
	Word eta;
	Word ema;
	Word epc;

	int cychack;	// for cycle entry past TP0
	u64 simtime;
	u64 realtime;

	// display
	int dcp;
	int dbx, dby;
	int dint;	// no direct schematics for this
	// simulation
//	int dpy_fd;
//	int dpy2_fd;
	u64 dpy_defl_time;
	u64 dpy_time;
//	u64 dpy_last;
//	u64 dpy2_last;
	DispCon dpy[2];

	// reader
	int rcp;
	int rb;
	int rc;
	int rby;
	int rcl;
	int rbs;
	// simulation
	int r_fd;
	u64 r_time;
	int rim_return;
	int rim_cycle;		// hack to trigger read-in SP1

	// punch
	int pcp;
	int pb;
	int punon;
	bool tape_feed;
	// simulation
	u64 p_time;
	u64 feed_time;
	int p_fd;

	// typewriter
	int tcp;
	int tb;
	int tbs;
	int tbb;
	int tyo;
	// simulation
	FD typ_fd;
	u64 typ_time;
	u64 tyi_wait;

	// spacewar controllers
	int spcwar1;
	int spcwar2;
    
        // wje - extra flags for cks, settable in IOTs
        int cksflags;
};

typedef struct PDP1 *PDP1P;

void updatelights(PDP1P pdp, Panel *panel);

#define IR pdp1P->ir
#define PC pdp1P->pc
#define MA pdp1P->ma
#define MB pdp1P->mb
#define AC pdp1P->ac
#define IO pdp1P->io

// 0
#define IR_AND (IR == 001)
#define IR_IOR (IR == 002)
#define IR_XOR (IR == 003)
#define IR_XCT (IR == 004)
// 5
// 6
#define IR_CALJDA (IR == 007)
#define IR_LAC (IR == 010)
#define IR_LIO (IR == 011)
#define IR_DAC (IR == 012)
#define IR_DAP (IR == 013)
#define IR_DIP (IR == 014)
#define IR_DIO (IR == 015)
#define IR_DZM (IR == 016)
// 17 - adm
#define IR_ADD (IR == 020)
#define IR_SUB (IR == 021)
#define IR_IDX (IR == 022)
#define IR_ISP (IR == 023)
#define IR_SAD (IR == 024)
#define IR_SAS (IR == 025)
#define IR_MUS (!pdp1P->muldiv_sw && (IR == 026))
#define IR_DIS (!pdp1P->muldiv_sw && (IR == 027))
#define IR_MUL (pdp1P->muldiv_sw && (IR == 026))
#define IR_DIV (pdp1P->muldiv_sw && (IR == 027))
#define IR_JMP (IR == 030)
#define IR_JSP (IR == 031)
#define IR_SKIP (IR == 032)
#define IR_SHRO (IR == 033)
#define IR_LAW (IR == 034)
#define IR_IOT (IR == 035)
// 36
#define IR_OPR (IR == 037)
#define IR_INCORR (IR==0 || IR==5 || IR==6 || IR==017 || IR==036)
//#define IR_INCORR (IR==5 || IR==6 || IR==017 || IR==036)


void pwrclr(PDP1P pdp1P);
void spec(PDP1P pdp1P);
void cycle(PDP1P pdp1P);
void start_readin(PDP1P pdp1P);
void readin1(PDP1P pdp1P);
void readin2(PDP1P pdp1P);
void handleio(PDP1P pdp1P);
void agedisplay(PDP1P pdp, int i);
void throttle(PDP1P pdp1P);
void cli(PDP1P pdp1P);
char *handlecmd(PDP1P pdp, char *line);

void typtelnet(int port, int fd);

void initaudio(void);
int isAudioInitialized(void);
void stopaudio(void);
void startaudio(void);
void continueaudio(void);
void svc_audio(PDP1P pdp1P);
void setSampleRate(int);
void setFilterAlpha(float);
void setFilter1Alpha(float);
void setFilter2Alpha(float);
void setFilter3Alpha(float);
void setFilter4Alpha(float);
float getFilterAlpha(void);
void setMixerGain(float);
float getMixerGain(void);
void setAudioTuning(float);
float getAudioTuning(void);
extern int doaudio;
