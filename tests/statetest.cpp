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
static int g_shortWrite = 0;
static SceUID sceIoOpen(const char *p, int fl, int) {
    FILE *f = fopen(p, (fl & SCE_O_WRONLY) ? "wb" : "rb");
    if (!f) return -1;
    for (int i = 1; i < 8; i++) if (!g_fh[i]) { g_fh[i] = f; return i; }
    fclose(f); return -1;
}
static int sceIoRead(SceUID fd, void *b, int n)  { return (int)fread(b,1,n,g_fh[fd]); }
static int sceIoWrite(SceUID fd, const void *b, int n){
    int w = (int)fwrite(b,1,n,g_fh[fd]);
    return g_shortWrite && w > 0 ? w - 1 : w;
}
static void sceIoClose(SceUID fd) { fclose(g_fh[fd]); g_fh[fd]=NULL; }
static int sceIoRemove(const char *p) { return remove(p); }
static int sceIoRename(const char *a, const char *b) { return rename(a,b); }
static void sceIoMkdir(const char *p, int) { char c[256]; snprintf(c,sizeof c,"mkdir -p %s",p); if(system(c)){} }

#define sce_paf_memset   memset
#define sce_paf_memcpy   memcpy
#define sce_paf_strncmp  strncmp
#define sce_paf_strtol   strtol
#define sce_paf_strlen   strlen
#define sceClibMemset    memset
#define sceClibStrncmp   strncmp
#define sceClibStrtoll   strtoll
#define DBG(msg)         ((void)0)
static int sce_paf_snprintf(char *b, size_t n, const char *f, ...) {
    va_list a; va_start(a,f); int r = vsnprintf(b,n,f,a); va_end(a); return r;
}
static int sceClibSnprintf(char *b, size_t n, const char *f, ...) {
    va_list a; va_start(a,f); int r = vsnprintf(b,n,f,a); va_end(a); return r;
}

static int32_t g_globalInterval = -1;
static int32_t g_hudMode = 0, g_hudPos = 0;
static int32_t g_lowLatency = 0, g_jit = 0, g_softcap = 0, g_tear = 0, g_advShown = 0, g_rateLog = 0;
static int32_t g_panelModes = 0, g_panelAuto = 0, g_tips = 1, g_syscon = 1, g_sticks = 1;
static bool fpsvitaPanelUi(void) { return g_panelModes != 0; }
#define FPSVITA_ADV_LOWLAT (1 << 0)
#define FPSVITA_ADV_JIT    (1 << 1)
#define FPSVITA_ADV_TEAR   (1 << 2)

/* Minimal stand-in for the real class: same member names/types. */
struct Impose {
    int32_t currentInterval, currentInject, autodetect;
    int32_t advFlags, softcap, currentPanel;
    char titleid[FPSVITA_TITLEID_LEN];
    bool hasGame;
    void PersistState();
    void SavePerGame();
};

#include "shell_io.inc"

/* companion side */
namespace comp {
typedef struct HelperConfig { int32_t pergame, interval, softcap; int inject, autodetect, lowlatency, jit, tear; int32_t pg_ll, pg_jit, pg_sc, pg_tear; int32_t pg_vsync; int pg_read; int32_t pg_panel; int log; int syscon; int sticks; int panelmodes; } HelperConfig;
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
    { /* A save may not produce a file the reader then refuses (BUF - 1 bytes). */
      const char *line = "PCSB00069=3,lowlat=0,antilag=0,limiter=0,tearing=0\n";
      for (int total = FPSVITA_STATE_BUF - 3; total <= FPSVITA_STATE_BUF - 1; total++) {
        std::string s;
        int fill = total - (int)strlen(line);
        while (fill - (int)s.size() > 40) { char l[32]; snprintf(l,sizeof l,"PCSX%05d=1\n",(int)s.size()); s+=l; }
        int pad = fill - (int)s.size() - 4;
        s += "zz="; s += std::string(pad, 'x'); s += "\n";
        put(s.c_str());
        { Impose o=mk("PCSB00069",3,0,1); o.SavePerGame(); }
        size_t after = strlen(get());
        { Impose o=mk("PCSB00069",2,0,1); o.SavePerGame(); }
        bool readable = strstr(get(),"PCSB00069=2,")!=NULL;
        char what[80];
        snprintf(what,sizeof what,"save to %d B: %s", total,
                 total <= FPSVITA_STATE_BUF - 2 ? "written, still saveable" : "refused");
        expect(what, total <= FPSVITA_STATE_BUF - 2 ? (after==(size_t)total && readable)
                                                    : (after==(size_t)fill && !readable));
      } }

    printf("== 5. Helper reads back what the shell wrote ==\n");
    putcfg("limit=2\nvsync=auto\nlowlat=1\n");
    put("PCSE00411=1\nPCSB00069=3\n");
    comp::HelperConfig c;
#define RGC(t) (comp::readGameConfig(t,&c), c.pergame)
    expect("per-game override read = 3", RGC("PCSB00069")==3);
    expect("lowlat read = 1", c.lowlatency==1);
    expect("global limit read = 2", c.interval==2);
    expect("vsync=auto -> autodetect, no forced vsync", c.autodetect==1 && c.inject==0);
    expect("antilag absent -> on, limiter absent -> 0", c.jit==1 && c.softcap==0);
    expect("other override read = 1", RGC("PCSE00411")==1);
    expect("absent title -> NO_PERGAME", RGC("PCSZ99999")==-99);
    expect("global key not seen as a titleid", RGC("limit")==-99);
    putcfg("limit=0\nvsync=1\nantilag=1\nlimiter=30\nhud=2\nhudpos=3\n");
    put("PCSE00411=1\n");
    expect("antilag, limiter, forced vsync read", RGC("PCSE00411")==1 && c.jit==1 && c.softcap==30 && c.interval==0 && c.inject==1 && c.autodetect==0);
    putcfg("vsync=0\n");
    expect("vsync=0 -> forced off", RGC("PCSE00411")==1 && c.inject==0 && c.autodetect==0);
    expect("no per-game options -> -1", c.pg_ll==-1 && c.pg_jit==-1 && c.pg_sc==-1 && c.pg_tear==-1);
    put("PCSE00411=2,lowlat=1,antilag=0,limiter=45,read=peek\nPCSB00069=3\n");
    expect("per-game options read", RGC("PCSE00411")==2 && c.pg_ll==1 && c.pg_jit==0 && c.pg_sc==45);
    put("PCSE00411=1,vsync=1\n");
    expect("per-game vsync=1 reads as forced on", RGC("PCSE00411")==1 && c.pg_vsync==2);
    put("PCSE00411=1,vsync=0\n");
    expect("per-game vsync=0 reads as forced off", RGC("PCSE00411")==1 && c.pg_vsync==1);
    put("PCSE00411=1,vsync=auto\n");
    expect("per-game vsync=auto reads as auto", RGC("PCSE00411")==1 && c.pg_vsync==0);
    put("PCSE00411=1\n");
    expect("no per-game vsync -> absent", RGC("PCSE00411")==1 && c.pg_vsync==-1);
    put("PCSE00411=2,lowlat=1,antilag=0,limiter=45,read=peek\nPCSB00069=3\n");
    expect("per-game read=peek read", RGC("PCSE00411")==2 && c.pg_read==1);
    expect("plain line unaffected", RGC("PCSB00069")==3 && c.pg_ll==-1);
    put("PCSE00411=1,vsync=1,read=peek\n");
    { Impose o=mk("PCSE00411",1,0,1); o.SavePerGame(); }
    expect("Save keeps vsync= and read=peek", strstr(get(),"PCSE00411=1,lowlat=0,antilag=0,limiter=0,tearing=0,vsync=1,read=peek\n")!=NULL);
    put("PCSE00411=1,read=peek,panel=40\n");
    { Impose o=mk("PCSE00411",1,0,1); o.SavePerGame(); }
    expect("Save keeps read=peek and panel=", strstr(get(),"PCSE00411=1,lowlat=0,antilag=0,limiter=0,tearing=0,read=peek,panel=40\n")!=NULL);
    expect("panel= ignored without experimental=1", RGC("PCSE00411")==1 && c.pg_panel==0 && c.pg_read==1);
    putcfg("experimental=1\n");
    expect("helper reads panel= with experimental=1", RGC("PCSE00411")==1 && c.pg_panel==40 && c.pg_read==1);
    expect("experimental= is a global, not a title", c.pergame==1);
    /* experimental=1: the FPS row owns panel=, written from the rate in force */
    g_panelModes = 1;
    { Impose o=mk("PCSE00411",1,0,1); o.currentPanel=45; o.SavePerGame(); }
    expect("Save writes the rate in force", strstr(get(),"PCSE00411=1,lowlat=0,antilag=0,limiter=0,tearing=0,read=peek,panel=45\n")!=NULL
                                            && count(get(),"panel=")==1);
    { Impose o=mk("PCSE00411",1,0,1); o.currentPanel=0; o.SavePerGame(); }
    expect("stock refresh drops panel=", strstr(get(),"PCSE00411=1,lowlat=0,antilag=0,limiter=0,tearing=0,read=peek\n")!=NULL);
    { Impose o=mk("PCSA00009",1,0,1); o.currentPanel=40; o.SavePerGame(); }
    expect("a new line gets panel=", strstr(get(),"PCSA00009=1,lowlat=0,antilag=0,limiter=0,tearing=0,panel=40\n")!=NULL);
    g_panelModes = 0;
    putcfg("");
    put("PCSE00411=1\n");
    expect("no panel= -> 0", RGC("PCSE00411")==1 && c.pg_panel==0);
    put("PCSB00069=3\nPCSE00411=1,read=peek\n");
    expect("titleReadsPeek finds the title's read=peek", titleReadsPeek("PCSE00411"));
    expect("and not another title's", !titleReadsPeek("PCSB00069") && !titleReadsPeek("PCSZ99999"));
    { Impose o=mk("PCSE00411",1,0,1); o.advFlags=FPSVITA_ADV_JIT|FPSVITA_ADV_TEAR; o.softcap=30; o.SavePerGame(); }
    expect("Save writes the options", strstr(get(),"PCSE00411=1,lowlat=0,antilag=1,limiter=30,tearing=1,read=peek\n")!=NULL);
    expect("and the helper reads them back", RGC("PCSE00411")==1 && c.pg_ll==0 && c.pg_jit==1 && c.pg_sc==30 && c.pg_tear==1 && c.pg_read==1);
    putcfg("antilag=auto\n");
    expect("earlier antilag=auto reads as on", RGC("PCSE00411")==1 && c.jit==1);
    put("PCSE00411=1,antilag=auto\n");
    expect("earlier per-game auto reads as on", RGC("PCSE00411")==1 && c.pg_jit==1);
    { Impose o=mk("PCSE00411",1,0,1); o.advFlags=FPSVITA_ADV_JIT; o.softcap=0; o.SavePerGame(); }
    expect("Save rewrites it as 1", strstr(get(),"PCSE00411=1,lowlat=0,antilag=1,limiter=0,tearing=0\n")!=NULL);
    g_jit = 1;
    { Impose o=mk("",2,0,1); o.PersistState(); }
    expect("config writes antilag=1", strstr(getcfg(),"antilag=1\n")!=NULL);
    g_jit = 0;
    putcfg("limit=2\nvsync=auto\nantilag=auto\n");
    int32_t iv=-9, inj=-9, ad=-9, hud=-9, pos=-9;
    expect("shell readGlobals succeeds", readGlobals(&iv,&inj,&ad,&hud,&pos));
    expect("shell reads earlier antilag=auto as on", g_jit==1 && iv==2 && ad==1);
    expect("no log= -> 0", RGC("PCSE00411")==1 && c.log==0 && g_rateLog==0);
    putcfg("limit=2\nlog=1\n");
    expect("shell reads log=1", readGlobals(&iv,&inj,&ad,&hud,&pos) && g_rateLog==1);
    { Impose o=mk("",2,0,1); o.PersistState(); }
    expect("config rewrite keeps log=1", strstr(getcfg(),"log=1\n")!=NULL);
    expect("helper reads log=1", RGC("PCSE00411")==1 && c.log==1);
    g_rateLog = 0;
    putcfg("limit=2\nexperimental=1\ntips=0\n");
    expect("shell reads the pre-0.4 experimental=1 as panelmodes, and tips=0", readGlobals(&iv,&inj,&ad,&hud,&pos) && g_panelModes==1 && g_tips==0);
    expect("helper reads experimental=1 as panelmodes", RGC("PCSE00411")==1 && c.panelmodes==1);
    { Impose o=mk("",2,0,1); o.PersistState(); }
    expect("config rewrite writes panelmodes=1 and tips=0", strstr(getcfg(),"panelmodes=1\npanelauto=0\ntips=0\n")!=NULL && strstr(getcfg(),"experimental=")==NULL);
    putcfg("limit=2\npanelmodes=1\npanelauto=1\n");
    expect("shell reads panelmodes=1 and panelauto=1", readGlobals(&iv,&inj,&ad,&hud,&pos) && g_panelModes==1 && g_panelAuto==1);
    expect("helper reads panelmodes=1", RGC("PCSE00411")==1 && c.panelmodes==1);
    g_panelModes = 0; g_panelAuto = 0; g_tips = 1;
    { Impose o=mk("",2,0,1); o.PersistState(); }
    expect("defaults written as panelmodes=0, panelauto=0 and tips=1", strstr(getcfg(),"panelmodes=0\npanelauto=0\ntips=1\n")!=NULL);
    expect("no syscon= -> 1 (Antilag's syscon presses on)", RGC("PCSE00411")==1 && c.syscon==1 && g_syscon==1);
    putcfg("limit=2\nsyscon=0\n");
    expect("shell reads syscon=0", readGlobals(&iv,&inj,&ad,&hud,&pos) && g_syscon==0);

    put("");
    expect("no sticks= -> 1 (early sticks on)", RGC("PCSE00411")==1 && c.sticks==1 && g_sticks==1);
    putf(FPSVITA_CONFIG_FILE, "limit=-1\nsticks=0\n");
    expect("shell reads sticks=0", readGlobals(&iv,&inj,&ad,&hud,&pos) && g_sticks==0);
    putf(FPSVITA_CONFIG_FILE, "limit=-1\n"); g_sticks = 1;
    { Impose o=mk("",2,0,1); o.PersistState(); }
    expect("config rewrite keeps syscon=0", strstr(getcfg(),"syscon=0\n")!=NULL);
    expect("helper reads syscon=0", RGC("PCSE00411")==1 && c.syscon==0);
    g_syscon = 1;

    printf("== 6. No config.txt: defaults, and nothing is read from state.txt ==\n");
    if(system("rm -f statedir/config.txt statedir/config.txt.bak")){}
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

    printf("== 8. Atomic saves and .bak fallback ==\n");
    put("PCSE00411=1\n");
    { Impose o=mk("PCSB00069",3,0,1); o.SavePerGame(); }
    expect("save keeps the previous file as .bak",
           strstr(getf("statedir/state.txt.bak"),"PCSE00411=1")!=NULL);
    {
        std::string before = get();
        g_shortWrite = 1;
        { Impose o=mk("PCSB00069",1,0,1); o.SavePerGame(); }
        g_shortWrite = 0;
        expect("short write leaves state.txt intact", get()==before);
        FILE *f = fopen("statedir/state.txt.tmp", "rb");
        expect("short write removes the temporary file", f == NULL);
        if (f) fclose(f);
    }
    putf("statedir/state.txt.bak", "PCSE00411=1,read=peek\n");
    if (remove("statedir/state.txt")) {}
    expect("shell reads .bak when state.txt is missing", titleReadsPeek("PCSE00411"));
    expect("helper reads .bak when state.txt is missing", RGC("PCSE00411")==1);
    putf("statedir/config.txt.bak", "limit=2\n");
    if (remove("statedir/config.txt")) {}
    {
        int32_t iv=-9, inj=-9, ad=-9, hud=-9, pos=-9;
        expect("shell reads config.txt.bak when config.txt is missing",
               readGlobals(&iv,&inj,&ad,&hud,&pos) && iv==2);
    }

    printf("\n%s (%d failures)\n", fails? "RESULT: FAILURES":"RESULT: ALL PASS", fails);
    return fails;
}
