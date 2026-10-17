#include <string>
/* Host harness: runs the REAL state.txt code from impose.cpp and
 * companion/main.c against a real file, with sce* shimmed to libc. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>

#define FPSVITA_INTERVAL_OFF (-1)
#define FPSVITA_TITLEID_LEN 12
#define FPSVITA_STATE_BUF (4096)
#define FPSVITA_NO_PERGAME (-99)
#define FPSVITA_STATE_DIR  "statedir"
#define FPSVITA_STATE_FILE "statedir/state.txt"
#define FPSVITA_CONFIG_FILE "statedir/config.txt"

typedef int SceUID;
#define SCE_O_RDONLY 1
#define SCE_O_WRONLY 2
#define SCE_O_CREAT  4
#define SCE_O_TRUNC  8

static FILE *g_fh[8];
static SceUID sceIoOpen(const char *p, int fl, int) {
    FILE *f = fopen(p, (fl & SCE_O_WRONLY) ? "wb" : "rb");
    if (!f) return -1;
    for (int i = 1; i < 8; i++) if (!g_fh[i]) { g_fh[i] = f; return i; }
    fclose(f); return -1;
}
static int sceIoRead(SceUID fd, void *b, int n)  { return (int)fread(b,1,n,g_fh[fd]); }
static int sceIoWrite(SceUID fd, const void *b, int n){ return (int)fwrite(b,1,n,g_fh[fd]); }
static void sceIoClose(SceUID fd) { fclose(g_fh[fd]); g_fh[fd]=NULL; }
static void sceIoMkdir(const char *p, int) { char c[256]; snprintf(c,sizeof c,"mkdir -p %s",p); if(system(c)){} }

#define sce_paf_memset   memset
#define sce_paf_memcpy   memcpy
#define sce_paf_strncmp  strncmp
#define sceClibMemset    memset
#define sceClibStrncmp   strncmp
#define sceClibStrtoll   strtoll
static int sce_paf_snprintf(char *b, size_t n, const char *f, ...) {
    va_list a; va_start(a,f); int r = vsnprintf(b,n,f,a); va_end(a); return r;
}

static int32_t g_globalInterval = -1;
static int32_t g_hudMode = 0, g_hudPos = 0;
static int32_t g_lowLatency = 0, g_jit = 0, g_softcap = 0, g_tear = 0, g_advShown = 0;
#define FPSVITA_ADV_LOWLAT (1 << 0)
#define FPSVITA_ADV_JIT    (1 << 1)
#define FPSVITA_ADV_TEAR   (1 << 2)

/* Minimal stand-in for the real class: same member names/types. */
struct Impose {
    int32_t currentInterval, currentInject, autodetect;
    int32_t advFlags, softcap;
    char titleid[FPSVITA_TITLEID_LEN];
    bool hasGame;
    void PersistState();
    void SavePerGame();
};

#include "shell_io.inc"

/* companion side */
namespace comp {
typedef struct HelperConfig { int32_t pergame, interval, softcap; int inject, autodetect, lowlatency, jit, tear; int32_t pg_ll, pg_jit, pg_sc, pg_tear; } HelperConfig;
#include "comp_io.inc"
}

/* ---- helpers ---- */
static void putf(const char *path, const char *s){ FILE*f=fopen(path,"wb"); fwrite(s,1,strlen(s),f); fclose(f); }
static const char *getf(const char *path){ static char b[8192]; FILE*f=fopen(path,"rb");
    if(!f){b[0]=0;return b;} size_t n=fread(b,1,sizeof b-1,f); b[n]=0; fclose(f); return b; }
static void put(const char *s){ putf(FPSVITA_STATE_FILE,s); }
static const char *get(){ return getf(FPSVITA_STATE_FILE); }
static void putcfg(const char *s){ putf(FPSVITA_CONFIG_FILE,s); }
static const char *getcfg(){ return getf(FPSVITA_CONFIG_FILE); }
static void show(const char *tag){ const char*s=get(); printf("  %-14s %3zu B  |", tag, strlen(s));
    for(const char*p=s;*p;p++) putchar(*p=='\n'?'~':*p); printf("|\n"); }
static Impose mk(const char*tid,int iv,int inj,int ad){ Impose o{}; o.currentInterval=iv;
    o.currentInject=inj; o.autodetect=ad; o.hasGame=tid&&*tid;
    if(tid) snprintf(o.titleid,sizeof o.titleid,"%s",tid); return o; }
static int count(const char *hay, const char *needle){ int n=0; for(const char*p=strstr(hay,needle);p;p=strstr(p+1,needle)) n++; return n; }

static int fails = 0;
static void expect(const char *what, bool ok){ printf("    %-58s %s\n", what, ok?"OK":"** FAIL **"); if(!ok) fails++; }

int main(){
    if(system("rm -rf statedir && mkdir -p statedir")){}
    printf("== 1. Globals in config.txt, per-game lines in state.txt ==\n");
    g_globalInterval = 2;
    { Impose o = mk("",2,0,1); o.PersistState(); }
    put("PCSE00411=1\n");
    { Impose o = mk("PCSB00069",3,0,1); o.SavePerGame(); } show("after save");
    expect("config.txt holds the globals", strstr(getcfg(),"limit=2\nvsync=auto\n")==getcfg());
    expect("state.txt has no globals", strstr(get(),"limit=")==NULL);
    expect("existing override kept", strstr(get(),"PCSE00411=1")!=NULL);
    expect("new override written",   strstr(get(),"PCSB00069=3,lowlat=0,antilag=0,limiter=0,tearing=0\n")!=NULL);
    expect("per-game save leaves config.txt alone", strstr(getcfg(),"limit=2")!=NULL);
    { Impose o = mk("",2,1,0); o.PersistState(); }
    expect("forced vsync written as a number", strstr(getcfg(),"vsync=1\n")!=NULL);

    printf("== 2. Repeated saves do not grow the file ==\n");
    size_t l1=strlen(get());
    { Impose o=mk("PCSB00069",2,0,1); o.SavePerGame(); }
    { Impose o=mk("PCSB00069",1,0,1); o.SavePerGame(); }
    size_t l3=strlen(get()); show("after x3");
    expect("length stable across saves", l1==l3);
    expect("override updated in place", strstr(get(),"PCSB00069=1,")!=NULL);

    printf("== 3. Unknown lines survive a rewrite; config keys are written once ==\n");
    put("future=7\nPCSE00411=1\n");
    { Impose o=mk("PCSB00069",3,0,1); o.SavePerGame(); } show("after save");
    expect("unknown line preserved", strstr(get(),"future=7")!=NULL);
    { Impose o=mk("",2,0,1); o.PersistState(); }
    expect("each config key once", count(getcfg(),"hud=")==1 && count(getcfg(),"lowlat=")==1 && count(getcfg(),"limit=")==1);

    printf("== 4. Oversized file: refuse to write rather than truncate ==\n");
    { std::string s;
      for(int i=0;i<400;i++){ char l[32]; snprintf(l,sizeof l,"PCSX%05d=1\n",i); s+=l; }
      put(s.c_str()); }
    size_t big=strlen(get());
    { Impose o=mk("PCSB00069",3,0,1); o.SavePerGame(); }
    expect("file untouched when it cannot be fully read", strlen(get())==big);
    printf("    (file is %zu B, buffer is %d B)\n", big, FPSVITA_STATE_BUF);

    printf("== 5. Helper reads back what the shell wrote ==\n");
    putcfg("limit=2\nvsync=auto\nlowlat=1\n");
    put("PCSE00411=1\nPCSB00069=3\n");
    comp::HelperConfig c;
#define RGC(t) (comp::readGameConfig(t,&c), c.pergame)
    expect("per-game override read = 3", RGC("PCSB00069")==3);
    expect("lowlat read = 1", c.lowlatency==1);
    expect("global limit read = 2", c.interval==2);
    expect("vsync=auto -> autodetect, no forced vsync", c.autodetect==1 && c.inject==0);
    expect("antilag/limiter absent -> 0", c.jit==0 && c.softcap==0);
    expect("other override read = 1", RGC("PCSE00411")==1);
    expect("absent title -> NO_PERGAME", RGC("PCSZ99999")==-99);
    expect("global key not seen as a titleid", RGC("limit")==-99);
    putcfg("limit=0\nvsync=1\nantilag=1\nlimiter=30\nhud=2\nhudpos=3\n");
    put("PCSE00411=1\n");
    expect("antilag, limiter, forced vsync read", RGC("PCSE00411")==1 && c.jit==1 && c.softcap==30 && c.interval==0 && c.inject==1 && c.autodetect==0);
    putcfg("vsync=0\n");
    expect("vsync=0 -> forced off", RGC("PCSE00411")==1 && c.inject==0 && c.autodetect==0);
    expect("no per-game options -> -1", c.pg_ll==-1 && c.pg_jit==-1 && c.pg_sc==-1 && c.pg_tear==-1);
    put("PCSE00411=2,lowlat=1,antilag=0,limiter=45\nPCSB00069=3\n");
    expect("per-game options read", RGC("PCSE00411")==2 && c.pg_ll==1 && c.pg_jit==0 && c.pg_sc==45);
    expect("plain line unaffected", RGC("PCSB00069")==3 && c.pg_ll==-1);
    { Impose o=mk("PCSE00411",1,0,1); o.advFlags=FPSVITA_ADV_JIT|FPSVITA_ADV_TEAR; o.softcap=30; o.SavePerGame(); }
    expect("Save writes the options", strstr(get(),"PCSE00411=1,lowlat=0,antilag=1,limiter=30,tearing=1\n")!=NULL);
    expect("and the helper reads them back", RGC("PCSE00411")==1 && c.pg_ll==0 && c.pg_jit==1 && c.pg_sc==30 && c.pg_tear==1);

    printf("== 6. No config.txt: defaults, and nothing is read from state.txt ==\n");
    if(system("rm -f statedir/config.txt")){}
    put("limit=3\nlowlat=1\nPCSE00411=1\n");
    expect("globals stay at defaults without config.txt", RGC("PCSE00411")==1 && c.interval==-1 && c.lowlatency==0);

    printf("== 7. Full round trip: shell writes, helper reads, repeat ==\n");
    if(system("rm -f statedir/state.txt statedir/config.txt")){}
    { Impose o=mk("",2,0,1); o.PersistState(); }
    { Impose o=mk("PCSA00001",3,0,1); o.SavePerGame(); }
    { Impose o=mk("PCSA00002",1,0,1); o.SavePerGame(); }
    show("final");
    expect("A -> 3", RGC("PCSA00001")==3);
    expect("B -> 1", RGC("PCSA00002")==1);
    expect("global 2", c.interval==2);

    printf("\n%s (%d failures)\n", fails? "RESULT: FAILURES":"RESULT: ALL PASS", fails);
    return fails;
}
