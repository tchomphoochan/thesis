/**********************************************************************
 * analyze_v2.c  –  Puppetmaster run analyser  (p-trimmed histogram)
 *
 *   gcc -O3 -std=c11 -pthread analyze_v2.c -o analyze_v2
 *
 * Stdout blocks:
 *   # LATENCY_CDF   lat_us,cdf_pct
 *   # LATENCY_HIST  lat_us,count
 *   # THROUGHPUT_TS time_ms,thr_txn_per_s
 *   # PUPPET_UTIL   puppet_id,util_pct
 *
 * Stderr: human summary with latency percentiles, per-puppet utilisation,
 *         client throughput, and optional ideal/efficiency lines.
 *********************************************************************/

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <pthread.h>

/* ───── Compile-time knobs ───────────────────────────────────────── */
#ifndef ENABLE_CONFLICT_CHECK
  #define ENABLE_CONFLICT_CHECK 1
#endif
#ifndef WARMUP_PCT
  #define WARMUP_PCT        0.10
#endif
#ifndef COOLDOWN_PCT
  #define COOLDOWN_PCT      0.10
#endif
#ifndef LAT_HIST_BUCKETS
  #define LAT_HIST_BUCKETS  20
#endif
#ifndef LAT_OUTLIER_P_LOW
  #define LAT_OUTLIER_P_LOW   0.02   /* keep ≥ 2 % quantile          */
#endif
#ifndef LAT_OUTLIER_P_HIGH
  #define LAT_OUTLIER_P_HIGH  0.98   /* keep ≤ 98 % quantile         */
#endif
#ifndef SLIDE_COUNT
  #define SLIDE_COUNT       100.0
#endif
#ifndef MAX_OBJS_PER_TXN
  #define MAX_OBJS_PER_TXN  32
#endif
#define NUM_THREADS         8

/* ───── Structures ──────────────────────────────────────────────── */
typedef struct { int reads [MAX_OBJS_PER_TXN], writes[MAX_OBJS_PER_TXN];
                 int num_reads, num_writes; } Transaction;

typedef enum { EVENT_SUBMIT, EVENT_SCHEDULED, EVENT_DONE } EventKind;
typedef struct { EventKind k; double ts; int txn; int puppet; } Event;
typedef struct { double sub, sch, done; int puppet; } TxnStat;

/* ───── Globals ─────────────────────────────────────────────────── */
static Transaction *txn_map = NULL;
static Event       *events  = NULL;
static TxnStat     *stats   = NULL;
static double      *busy_puppet = NULL;

static size_t n_txn = 0, n_evt = 0;
static int    n_puppets = 0;
static double work_us   = 0.0;

static double submit_start=-1, submit_end=-1, wall_start=-1, wall_end=-1;

/* dynamic histogram parms */
static double hist_min=0.0, hist_step=1.0;
static uint64_t lat_hist[LAT_HIST_BUCKETS];

/* ───── Utility: slurp file into lines ──────────────────────────── */
static char **read_lines(const char *path, size_t *n)
{
    FILE *fp = fopen(path,"r");
    if(!fp){perror(path);exit(1);}
    size_t cap=1024,c=0; char **v=malloc(cap*sizeof(char*));
    char *buf=NULL; size_t len=0;
    while(getline(&buf,&len,fp)!=-1){
        if(c==cap){cap*=2;v=realloc(v,cap*sizeof(char*));}
        v[c++]=strdup(buf);
    }
    free(buf); fclose(fp); *n=c; return v;
}

/* ───── Parallel helpers ───────────────────────────────────────── */
typedef struct { char **lines; int lo, hi; } Task;

/* ---------- parse transactions.csv ---------- */
static void *txn_worker(void *arg)
{
    Task *t=(Task*)arg;
    for(int i=t->lo;i<t->hi;i++){
        char *ln=t->lines[i]; if(!ln||ln[0]=='\0')continue;
        Transaction *tx=&txn_map[i];
        tx->num_reads=tx->num_writes=0;
        char *tok=strtok(ln,",");          /* skip id */
        while((tok=strtok(NULL,","))){
            int obj=atoi(tok);
            tok=strtok(NULL,","); if(!tok)break;
            int wr=atoi(tok);
            if(wr) tx->writes[tx->num_writes++]=obj;
            else   tx->reads [tx->num_reads++ ]=obj;
        }
    }
    return NULL;
}
static void parse_txns(const char *path)
{
    size_t n; char **lines=read_lines(path,&n);
    n_txn=n; txn_map=calloc(n_txn,sizeof(Transaction));
    pthread_t th[NUM_THREADS]; Task tk[NUM_THREADS];
    int chunk=(n+NUM_THREADS-1)/NUM_THREADS;
    for(int i=0;i<NUM_THREADS;i++){
        tk[i]=(Task){lines,i*chunk,(i+1)*chunk>n?n:(i+1)*chunk};
        pthread_create(&th[i],NULL,txn_worker,&tk[i]);
    }
    for(int i=0;i<NUM_THREADS;i++)pthread_join(th[i],NULL);
    for(size_t i=0;i<n;i++)free(lines[i]); free(lines);
}

/* ---------- parse run.log ---------- */
static void *log_worker(void *arg)
{
    Task *t=(Task*)arg;
    for(int i=t->lo;i<t->hi;i++){
        char *ln=t->lines[i]; if(!ln||ln[0]=='\0')continue;
        if(strstr(ln,"submit txn id=")){
            double ts;int id; sscanf(ln,"[+%lf] submit txn id=%d",&ts,&id);
            events[i]=(Event){EVENT_SUBMIT,ts,id,-1};
        }else if(strstr(ln,"scheduled txn id=")){
            double ts;int id,p;
            sscanf(ln,"[+%lf] scheduled txn id=%d assigned to puppet %d",&ts,&id,&p);
            events[i]=(Event){EVENT_SCHEDULED,ts,id,p};
        }else if(strstr(ln,"done puppet")){
            double ts;int p,id;
            sscanf(ln,"[+%lf] done puppet %d finished txn id=%d",&ts,&p,&id);
            events[i]=(Event){EVENT_DONE,ts,id,p};
        }else{fprintf(stderr,"Bad log: %s",ln);exit(1);}
    }
    return NULL;
}
static void parse_log(const char *p)
{
    size_t n; char **lines=read_lines(p,&n);
    n_evt=n; events=calloc(n_evt,sizeof(Event));
    pthread_t th[NUM_THREADS]; Task tk[NUM_THREADS];
    int chunk=(n+NUM_THREADS-1)/NUM_THREADS;
    for(int i=0;i<NUM_THREADS;i++){
        tk[i]=(Task){lines,i*chunk,(i+1)*chunk>n?n:(i+1)*chunk};
        pthread_create(&th[i],NULL,log_worker,&tk[i]);
    }
    for(int i=0;i<NUM_THREADS;i++)pthread_join(th[i],NULL);
    for(size_t i=0;i<n;i++)free(lines[i]); free(lines);
}

/* ───── Conflict / ordering check ──────────────────────────────── */
static void correctness_check(void)
{
#if ENABLE_CONFLICT_CHECK
    bool *sub=calloc(n_txn,sizeof(bool));
    bool *sch=calloc(n_txn,sizeof(bool));
    bool *don=calloc(n_txn,sizeof(bool));
    int  *active=calloc(n_txn,sizeof(int));
    for(size_t i=0;i<n_evt;i++){
        Event *e=&events[i];
        switch(e->k){
        case EVENT_SUBMIT:
            if(sub[e->txn]){fprintf(stderr,"dup submit %d\n",e->txn);exit(1);}
            sub[e->txn]=1;
            if(submit_start<0||e->ts<submit_start)submit_start=e->ts;
            if(submit_end  <0||e->ts>submit_end )submit_end =e->ts;
            break;
        case EVENT_SCHEDULED:
            if(!sub[e->txn]){fprintf(stderr,"sched before sub %d\n",e->txn);exit(1);}
            if(sch[e->txn]){fprintf(stderr,"dup sched %d\n",e->txn);exit(1);}
            sch[e->txn]=1; active[e->txn]=e->puppet;
            break;
        case EVENT_DONE:
            if(!sch[e->txn]){fprintf(stderr,"done before sched %d\n",e->txn);exit(1);}
            if(don[e->txn]){fprintf(stderr,"dup done %d\n",e->txn);exit(1);}
            if(active[e->txn]!=e->puppet){fprintf(stderr,"puppet mismatch\n");exit(1);}
            don[e->txn]=1;
            busy_puppet[e->puppet]+=work_us*1e-6;
            if(wall_start<0||e->ts<wall_start)wall_start=e->ts;
            if(wall_end  <0||e->ts>wall_end )wall_end =e->ts;
            break;
        }
    }
    fprintf(stderr,"✅ All consistency checks passed!\n\n");
    free(sub);free(sch);free(don);free(active);
#else
    submit_start=events[0].ts; submit_end=events[n_evt-1].ts;
    wall_start=submit_start; wall_end=events[n_evt-1].ts;
#endif
}

/* ───── Histogram helpers ───────────────────────────────────────── */
static inline int bucket_of(double us){
    int b=(int)((us-hist_min)/hist_step);
    if(b<0)b=0; if(b>=LAT_HIST_BUCKETS)b=LAT_HIST_BUCKETS-1;
    return b;
}
static void dump_latency_blocks(size_t kept)
{
    printf("# LATENCY_CDF\nlat_us,cdf_pct\n");
    uint64_t run=0;
    for(int b=0;b<LAT_HIST_BUCKETS;b++){
        run+=lat_hist[b];
        double pct=(double)run/kept*100.0;
        double mid=hist_min+(b+0.5)*hist_step;
        printf("%g,%g\n",mid,pct);
    } puts("");
    printf("# LATENCY_HIST\nlat_us,count\n");
    for(int b=0;b<LAT_HIST_BUCKETS;b++){
        double mid=hist_min+(b+0.5)*hist_step;
        printf("%g,%lu\n",mid,(unsigned long)lat_hist[b]);
    } puts("");
}

/* ───── Analysis ───────────────────────────────────────────────── */
static int cmp_d(const void *a,const void *b){double x=*(double*)a,y=*(double*)b;return (x>y)-(x<y);}
static void analyse(void)
{
    stats=calloc(n_txn,sizeof(TxnStat));
    for(size_t i=0;i<n_evt;i++){
        Event *e=&events[i]; TxnStat *s=&stats[e->txn];
        if(e->k==EVENT_SUBMIT) s->sub=e->ts;
        else if(e->k==EVENT_SCHEDULED){s->sch=e->ts; s->puppet=e->puppet;}
        else if(e->k==EVENT_DONE)      s->done=e->ts;
    }

    double span=stats[n_txn-1].done - stats[0].sub;
    double t_lo=stats[0].sub + WARMUP_PCT*span;
    double t_hi=stats[n_txn-1].done - COOLDOWN_PCT*span;
    double window=t_hi - t_lo;

    /* after you know `window` (steady-state span in seconds) */
    double desired_ms = (window * 1000.0) / SLIDE_COUNT;     /* 500 ≈ screen-friendly */
    if (desired_ms < 1.0)   desired_ms = 1.0;
    if (desired_ms > 100.0) desired_ms = 100.0;

    /* first pass for arrays */
    size_t cap_lat=1024, n_lat=0;
    double *lat_vals=malloc(cap_lat*sizeof(double));

    size_t thr_cap=(size_t)(window*1000/desired_ms)+2;
    double *thr_ts=calloc(thr_cap,sizeof(double));
    double *thr_v =calloc(thr_cap,sizeof(double));
    size_t thr_n=0;

    memset(busy_puppet,0,n_puppets*sizeof(double));

    for(size_t id=0;id<n_txn;id++){
        TxnStat *s=&stats[id];
        if(s->sub<t_lo||s->done>t_hi)continue;
        double e2e=(s->done - s->sub)*1e6;

        if(n_lat==cap_lat){cap_lat*=2; lat_vals=realloc(lat_vals,cap_lat*sizeof(double));}
        lat_vals[n_lat++]=e2e;

        busy_puppet[s->puppet]+= (s->done - s->sch);
        size_t idx=(size_t)(((s->done - t_lo)*1000)/desired_ms);
        thr_v[idx]+=1.0;
    }
    if(n_lat==0){fprintf(stderr,"No txns in window\n");exit(1);}

    qsort(lat_vals,n_lat,sizeof(double),cmp_d);
    size_t i_low  =(size_t)(LAT_OUTLIER_P_LOW  * n_lat);
    size_t i_high =(size_t)(LAT_OUTLIER_P_HIGH * n_lat);
    if(i_high>=n_lat) i_high=n_lat-1;

    hist_min   = lat_vals[i_low];
    double hist_max = lat_vals[i_high];
    hist_step  = (hist_max - hist_min)/LAT_HIST_BUCKETS;
    if(hist_step<1e-3) hist_step=1e-3;

    /* second pass: fill histogram & clamp counts */
    size_t low_clamp=0, hi_clamp=0;
    for(size_t j=0;j<n_lat;j++){
        double v=lat_vals[j];
        if(v<hist_min){low_clamp++; v=hist_min;}
        else if(v>hist_min+hist_step*LAT_HIST_BUCKETS){hi_clamp++; v=hist_min+hist_step*(LAT_HIST_BUCKETS-1);}
        lat_hist[bucket_of(v)]++;
    }

    /* compact throughput */
    for(size_t i=0;i<thr_cap;i++){
        if(!thr_v[i])continue;
        thr_ts[thr_n]=i*desired_ms;
        thr_v [thr_n]=thr_v[i]/(desired_ms/1000.0);
        ++thr_n;
    }

    /* percentiles from sorted list */
    double p50=lat_vals[(size_t)(0.50*n_lat)];
    double p90=lat_vals[(size_t)(0.90*n_lat)];
    double p99=lat_vals[(size_t)(0.99*n_lat)];

    /* ── CSV ── */
    dump_latency_blocks(n_lat);
    printf("# THROUGHPUT_TS slide_ms=%g\n", desired_ms);
    printf("time_ms,thr_txn_per_s\n");
    for(size_t i=0;i<thr_n;i++)printf("%g,%g\n",thr_ts[i],thr_v[i]); puts("");
    printf("# PUPPET_UTIL\npuppet_id,util_pct\n");
    for(int p=0;p<n_puppets;p++)printf("%d,%g\n",p,busy_puppet[p]/window*100.0);
    puts("");

    /* ── Summary ── */
    fprintf(stderr,
        "=== Steady-state (trim %.0f/%.0f %%) ===\n"
        "  window: %.3f s   (%zu / %zu txns)\n\n"
        "Latency (µs, e2e)  p50 %.2f   p90 %.2f   p99 %.2f\n"
        "Histogram fences:  %.2f – %.2f µs  (clamped %zu low, %zu high)\n\n",
        WARMUP_PCT*100,COOLDOWN_PCT*100,window,n_lat,n_txn,
        p50,p90,p99,hist_min,hist_min+hist_step*LAT_HIST_BUCKETS,low_clamp,hi_clamp);

    fprintf(stderr,"Puppet utilisation:\n");
    for(int p=0;p<n_puppets;p++)
        fprintf(stderr,"  Puppet %d: %.2f %%\n",p,busy_puppet[p]/window*100.0);

    double sub_span=submit_end-submit_start;
    if(sub_span>0){
        double sub_rate=n_txn/sub_span;
        fprintf(stderr,
            "\nThroughput:\n"
            "  Client submission rate : %.2f M txn/s\n",
            sub_rate/1e6);
        if(work_us>0.0){
            double ideal=n_puppets*(1e6/work_us);
            fprintf(stderr,
                "  Ideal (no stalls)      : %.2f M txn/s\n"
                "  Efficiency             : %.1f %%\n",
                ideal/1e6, sub_rate/ideal*100.0);
        }
    }
    double wall=wall_end-wall_start;
    fprintf(stderr,"\nWall-clock time: %.3f s\n",wall);
    if(work_us>0.0&&n_puppets>0){
        double ideal_t=(double)n_txn*work_us/1e6/n_puppets;
        fprintf(stderr,"Ideal time     : %.3f s   (eff %.1f %%)\n",
                ideal_t, ideal_t/wall*100.0);
    }

    free(lat_vals); free(thr_ts); free(thr_v);
}

/* ───── Main ───────────────────────────────────────────────────── */
int main(int argc,char**argv)
{
    if(argc!=5){
        fprintf(stderr,"Usage: %s TXNS.csv RUN.log NUM_PUPPETS WORK_US\n",argv[0]);
        return 1;
    }
    n_puppets=atoi(argv[3]); work_us=atof(argv[4]);
    busy_puppet=calloc(n_puppets,sizeof(double));

    parse_txns(argv[1]);
    parse_log(argv[2]);

    correctness_check();
    analyse();

    free(txn_map);free(events);free(stats);free(busy_puppet);
    return 0;
}

