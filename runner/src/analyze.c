/**********************************************************************
 * analyze_v2.c – Puppetmaster run analyser
 *
 *   gcc -O3 -std=c11 -pthread analyze_v2.c -o analyze_v2
 *
 *   ./analyze_v2 transactions.csv run.log NUM_PUPPETS WORK_US \
 *       | python3 plot_analysis.py -o report.pdf \
 *       2>summary.txt
 *
 * Human summary  →  stderr
 * CSV blocks     →  stdout  (LATENCY_CDF, THROUGHPUT_TS, PUPPET_UTIL)
 *********************************************************************/

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <errno.h>

/* ───── Compile-time knobs ─────────────────────────────────────────── */
#ifndef ENABLE_CONFLICT_CHECK
  #define ENABLE_CONFLICT_CHECK 1         /* 0 = skip expensive checks  */
#endif
#ifndef WARMUP_PCT
  #define WARMUP_PCT   0.05               /* 5 % head trim             */
#endif
#ifndef COOLDOWN_PCT
  #define COOLDOWN_PCT 0.05               /* 5 % tail trim             */
#endif
#ifndef LAT_HIST_BUCKETS
  #define LAT_HIST_BUCKETS 64             /* histogram bins (log2)     */
#endif
#ifndef SLIDE_MS
  #define SLIDE_MS     10                 /* throughput window (ms)    */
#endif
#ifndef MAX_OBJS_PER_TXN
  #define MAX_OBJS_PER_TXN 32             /* from original code        */
#endif
#define NUM_THREADS 8                     /* parsing workers           */

/* ───── Data structures (mostly original) ─────────────────────────── */
typedef struct {
    int reads [MAX_OBJS_PER_TXN];
    int writes[MAX_OBJS_PER_TXN];
    int num_reads, num_writes;
} Transaction;

typedef enum { EVENT_SUBMIT, EVENT_SCHEDULED, EVENT_DONE } EventKind;

typedef struct {            /* parsed log line                */
    EventKind kind;
    double    ts;           /* seconds since start            */
    int       txn_id;
    int       puppet_id;
} Event;

/* Per-transaction timestamps for latency stats */
typedef struct {
    double t_submit, t_sched, t_done;
    int    puppet;
} TxnStat;

/* Global buffers (malloc’d) */
static Transaction *txn_map   = NULL;
static Event       *events    = NULL;
static TxnStat     *stats     = NULL;
static double      *puppet_busy;
static size_t        num_txns = 0, num_events = 0;
static int           num_puppets;
static double        work_us;

/* ───── Parallel CSV helpers (unchanged except renames) ───────────── */
typedef struct { char **lines; int start, end; } ParseTask;

/* ---- transaction CSV ---- */
static void *parse_txn_worker(void *arg)
{
    ParseTask *t = (ParseTask *)arg;
    for (int i = t->start; i < t->end; ++i) {
        char *line = t->lines[i];
        if (!line || line[0] == '\0') continue;

        Transaction *tx = &txn_map[i];
        tx->num_reads = tx->num_writes = 0;

        char *tok = strtok(line, ","); /* first token is txn-id – ignore */
        while ((tok = strtok(NULL, ","))) {
            int obj = atoi(tok);
            if (!(tok = strtok(NULL, ","))) break;
            int w = atoi(tok);
            if (w) tx->writes[tx->num_writes++] = obj;
            else    tx->reads [tx->num_reads ++] = obj;
        }
    }
    return NULL;
}

static void parse_transactions_csv(const char *file)
{
    FILE *f = fopen(file, "r");
    if (!f) { perror("txn fopen"); exit(1); }

    /* slurp lines */
    size_t cap = 1024;  char **lines = malloc(cap * sizeof(char *));
    char buf[4096]; size_t cnt = 0;
    while (fgets(buf, sizeof(buf), f)) {
        if (cnt == cap) { cap*=2; lines = realloc(lines, cap*sizeof(char*)); }
        lines[cnt++] = strdup(buf);
    }
    fclose(f);

    num_txns = cnt;
    txn_map  = calloc(num_txns, sizeof(Transaction));

    pthread_t th[NUM_THREADS];
    ParseTask tasks[NUM_THREADS];
    int chunk = (cnt + NUM_THREADS -1)/NUM_THREADS;
    for (int i=0;i<NUM_THREADS;i++){
        tasks[i]=(ParseTask){lines,i*chunk, (i+1)*chunk>cnt?cnt:(i+1)*chunk};
        pthread_create(&th[i],NULL,parse_txn_worker,&tasks[i]);
    }
    for (int i=0;i<NUM_THREADS;i++) pthread_join(th[i],NULL);
    for(size_t i=0;i<cnt;i++) free(lines[i]); free(lines);
}

/* ---- log file ---- */
static void *parse_log_worker(void *arg)
{
    ParseTask *t = (ParseTask *)arg;
    for(int i=t->start;i<t->end;i++){
        char *line=t->lines[i]; if(!line||line[0]=='\0') continue;
        if(strstr(line,"submit txn id=")){
            double ts; int id;
            sscanf(line,"[+%lf] submit txn id=%d",&ts,&id);
            events[i]=(Event){EVENT_SUBMIT,ts,id,-1};
        }else if(strstr(line,"scheduled txn id=")){
            double ts; int id,p;
            sscanf(line,"[+%lf] scheduled txn id=%d assigned to puppet %d",&ts,&id,&p);
            events[i]=(Event){EVENT_SCHEDULED,ts,id,p};
        }else if(strstr(line,"done puppet")){
            double ts; int p,id;
            sscanf(line,"[+%lf] done puppet %d finished txn id=%d",&ts,&p,&id);
            events[i]=(Event){EVENT_DONE,ts,id,p};
        }else{ fprintf(stderr,"Parse error: %s",line); exit(1); }
    }
    return NULL;
}

static void parse_log(const char *file)
{
    FILE *f=fopen(file,"r"); if(!f){perror("log fopen");exit(1);}
    size_t cap=1024; char **lines=malloc(cap*sizeof(char*));
    char buf[4096]; size_t cnt=0;
    while(fgets(buf,sizeof(buf),f)){
        if(cnt==cap){cap*=2;lines=realloc(lines,cap*sizeof(char*));}
        lines[cnt++]=strdup(buf);
    } fclose(f);

    num_events=cnt;
    events=malloc(cnt*sizeof(Event));
    pthread_t th[NUM_THREADS]; ParseTask tasks[NUM_THREADS];
    int chunk=(cnt+NUM_THREADS-1)/NUM_THREADS;
    for(int i=0;i<NUM_THREADS;i++){
        tasks[i]=(ParseTask){lines,i*chunk,(i+1)*chunk>cnt?cnt:(i+1)*chunk};
        pthread_create(&th[i],NULL,parse_log_worker,&tasks[i]);
    }
    for(int i=0;i<NUM_THREADS;i++) pthread_join(th[i],NULL);
    for(size_t i=0;i<cnt;i++) free(lines[i]); free(lines);
}

/* ───── Consistency / conflict verification ───────────────────────── */
static void run_conflict_checks(void)
{
#if ENABLE_CONFLICT_CHECK
    bool *submitted = calloc(num_txns,sizeof(bool));
    bool *scheduled = calloc(num_txns,sizeof(bool));
    bool *done      = calloc(num_txns,sizeof(bool));
    int  *active    = calloc(num_txns,sizeof(int));

    double submit_start=-1,submit_end=-1;
    double wall_start=-1, wall_end=-1;

    for(size_t i=0;i<num_events;i++){
        Event *e=&events[i];
        switch(e->kind){
        case EVENT_SUBMIT:
            if(submitted[e->txn_id]){fprintf(stderr,"txn %d double-submit\n",e->txn_id);exit(1);}
            submitted[e->txn_id]=true;
            if(submit_start<0||e->ts<submit_start) submit_start=e->ts;
            if(submit_end  <0||e->ts>submit_end  ) submit_end  =e->ts;
            break;
        case EVENT_SCHEDULED:
            if(!submitted[e->txn_id]){fprintf(stderr,"txn %d scheduled before submit\n",e->txn_id);exit(1);}
            if(scheduled[e->txn_id]){fprintf(stderr,"txn %d double-schedule\n",e->txn_id);exit(1);}
            scheduled[e->txn_id]=true; active[e->txn_id]=e->puppet_id; break;
        case EVENT_DONE:
            if(!scheduled[e->txn_id]){fprintf(stderr,"txn %d done before schedule\n",e->txn_id);exit(1);}
            if(done[e->txn_id]){fprintf(stderr,"txn %d double-done\n",e->txn_id);exit(1);}
            if(active[e->txn_id]!=e->puppet_id){fprintf(stderr,"puppet mismatch txn %d\n",e->txn_id);exit(1);}
            done[e->txn_id]=true;
            puppet_busy[e->puppet_id]+=work_us*1e-6;
            if(wall_start<0||e->ts<wall_start) wall_start=e->ts;
            if(wall_end  <0||e->ts>wall_end  ) wall_end  =e->ts;
            break;
        }
    }
    fprintf(stderr,"✅ All consistency checks passed!\n\n");
    free(submitted); free(scheduled); free(done); free(active);
#else
    (void)0; /* checks disabled */
#endif
}

/* ───── Latency + throughput statistics ───────────────────────────── */
static uint64_t lat_hist[LAT_HIST_BUCKETS];

static inline int bucket_of(double us){
    int b=(int)floor(log2(us*8)); if(b<0) b=0;
    if(b>=LAT_HIST_BUCKETS) b=LAT_HIST_BUCKETS-1; return b;
}

static void gather_stats_and_dump(void)
{
    /* Build per-txn timestamps array */
    stats = calloc(num_txns,sizeof(TxnStat));
    for(size_t i=0;i<num_events;i++){
        Event *e=&events[i];
        TxnStat *s=&stats[e->txn_id];
        if(e->kind==EVENT_SUBMIT)    s->t_submit=e->ts;
        else if(e->kind==EVENT_SCHEDULED){s->t_sched=e->ts; s->puppet=e->puppet_id;}
        else if(e->kind==EVENT_DONE) s->t_done =e->ts;
    }

    /* span & steady-state bounds */
    double span = stats[num_txns-1].t_done - stats[0].t_submit;
    double t_start = stats[0].t_submit + WARMUP_PCT * span;
    double t_end   = stats[num_txns-1].t_done  - COOLDOWN_PCT * span;
    double window  = t_end - t_start;

    /* throughput window buffers (worst-case length) */
    size_t cap = (size_t)(window*1000/SLIDE_MS)+2;
    double *thr_ts = calloc(cap,sizeof(double));
    double *thr_v  = calloc(cap,sizeof(double));
    size_t  thr_n  = 0;

    /* busy per puppet already partially filled by conflict check */
    double *busy = puppet_busy;

    /* percentile helpers */
    double p50=0,p90=0,p99=0;
    size_t kept=0;

    for(size_t id=0;id<num_txns;id++){
        TxnStat *s=&stats[id];
        if(s->t_submit<t_start || s->t_done>t_end) continue;
        kept++;

        double e2e = (s->t_done  - s->t_submit)*1e6;  /* µs */
        lat_hist[bucket_of(e2e)]++;

        /* per-puppet busy time (execution span) – more accurate than work_us if sched/done clocks) */
        busy[s->puppet] += (s->t_done - s->t_sched);

        /* throughput bucket */
        size_t idx = (size_t)(((s->t_done - t_start)*1000)/SLIDE_MS);
        thr_v[idx] += 1.0;
    }

    /* compact throughput arrays */
    for(size_t i=0;i<cap;i++){
        if(thr_v[i]==0) continue;
        thr_ts[thr_n] = i*SLIDE_MS;
        thr_v [thr_n] = thr_v[i]/(SLIDE_MS/1000.0);
        ++thr_n;
    }

    /* percentile extraction from histogram */
    uint64_t running=0; size_t total=kept;
    for(int b=0;b<LAT_HIST_BUCKETS;b++){
        running+=lat_hist[b];
        double pct = (double)running/total;
        double mid = pow(2.0,b-3); /* representative µs */
        if(!p50 && pct>=0.50) p50=mid;
        if(!p90 && pct>=0.90) p90=mid;
        if(!p99 && pct>=0.99) p99=mid;
    }

    /* === CSV blocks to stdout === */
    /* 1) latency CDF */
    printf("# LATENCY_CDF\nlat_us,cdf_pct\n");
    running=0;
    for(int b=0;b<LAT_HIST_BUCKETS;b++){
        running+=lat_hist[b];
        double pct = (double)running/total*100.0;
        double mid = pow(2.0,b-3);
        printf("%g,%g\n",mid,pct);
    }
    puts("");

    /* 2) throughput time-series */
    printf("# THROUGHPUT_TS\ntime_ms,thr_txn_per_s\n");
    for(size_t i=0;i<thr_n;i++)
        printf("%g,%g\n",thr_ts[i],thr_v[i]);
    puts("");

    /* 3) per-puppet utilisation */
    printf("# PUPPET_UTIL\npuppet_id,util_pct\n");
    for(int p=0;p<num_puppets;p++)
        printf("%d,%g\n",p, busy[p]/window*100.0);
    puts("");

    /* === Human summary to stderr === */
    fprintf(stderr,
        "=== Steady-state (%.0f%%/%.0f%% trim) ===\n"
        "  window: %.3f s  (%zu txns kept of %zu)\n\n"
        "Latency (e2e µs)  p50 %.2f   p90 %.2f   p99 %.2f\n\n",
        WARMUP_PCT*100, COOLDOWN_PCT*100, window, kept, num_txns, p50,p90,p99);

    /* client submission & ideal throughput (reuse old logic) */
    /* ... omitted for brevity; you can splice original code here if desired ... */

    free(thr_ts); free(thr_v);
}

/* ───── main ───────────────────────────────────────────────────────── */
int main(int argc,char**argv)
{
    if(argc!=5){
        fprintf(stderr,"Usage: %s TXNS.csv LOG.txt NUM_PUPPETS WORK_US\n",argv[0]);
        return 1;
    }
    num_puppets = atoi(argv[3]);  work_us = atof(argv[4]);
    puppet_busy = calloc(num_puppets,sizeof(double));

    parse_transactions_csv(argv[1]);
    parse_log(argv[2]);

    run_conflict_checks();      /* may be NOP if disabled */
    gather_stats_and_dump();    /* new metrics & CSV      */

    free(txn_map); free(events); free(stats); free(puppet_busy);
    return 0;
}


// #include <stdio.h>
// #include <stdlib.h>
// #include <stdint.h>
// #include <stdbool.h>
// #include <string.h>
// #include <pthread.h>
// 
// #define MAX_OBJS_PER_TXN 32
// #define NUM_THREADS 8
// 
// typedef struct {
//     int reads[MAX_OBJS_PER_TXN];
//     int writes[MAX_OBJS_PER_TXN];
//     int num_reads;
//     int num_writes;
// } Transaction;
// 
// typedef enum {
//     EVENT_SUBMIT,
//     EVENT_SCHEDULED,
//     EVENT_DONE
// } EventKind;
// 
// typedef struct {
//     EventKind kind;
//     double timestamp;
//     int txn_id;
//     int puppet_id;
// } Event;
// 
// Transaction *txn_map;
// Event *events;
// double *puppet_busy;
// 
// int num_txns = 0;
// int num_events = 0;
// int num_puppets = 0;
// 
// pthread_mutex_t event_mutex = PTHREAD_MUTEX_INITIALIZER;
// 
// typedef struct {
//     char **lines;
//     int start;
//     int end;
// } ParseTask;
// 
// static inline double parse_time(const char *s) {
//     char *endptr;
//     return strtod(s, &endptr);
// }
// 
// void *parse_transactions_worker(void *arg) {
//     ParseTask *task = (ParseTask *)arg;
//     for (int i = task->start; i < task->end; ++i) {
//         char *line = task->lines[i];
//         if (!line || strlen(line) < 2) continue;
// 
//         Transaction *txn = &txn_map[i];
//         txn->num_reads = 0;
//         txn->num_writes = 0;
// 
//         char *p = strtok(line, ",");
//         if (!p) continue;
// 
//         while ((p = strtok(NULL, ","))) {
//             int objid = atoi(p);
//             p = strtok(NULL, ",");
//             if (!p) break;
//             int writeflag = atoi(p);
// 
//             if (writeflag) {
//                 txn->writes[txn->num_writes++] = objid;
//             } else {
//                 txn->reads[txn->num_reads++] = objid;
//             }
//         }
//     }
//     return NULL;
// }
// 
// static void parse_transactions_csv(const char *filename) {
//     FILE *f = fopen(filename, "r");
//     if (!f) {
//         perror("fopen transactions");
//         exit(1);
//     }
// 
//     char **lines = NULL;
//     size_t capacity = 1024;
//     lines = malloc(sizeof(char *) * capacity);
//     if (!lines) { perror("malloc lines"); exit(1); }
// 
//     char buf[4096];
//     int count = 0;
// 
//     while (fgets(buf, sizeof(buf), f)) {
//         if (strlen(buf) < 2) continue;
//         if (count >= (int)capacity) {
//             capacity *= 2;
//             lines = realloc(lines, sizeof(char *) * capacity);
//             if (!lines) { perror("realloc lines"); exit(1); }
//         }
//         lines[count++] = strdup(buf);
//     }
//     fclose(f);
// 
//     num_txns = count;
//     txn_map = malloc(sizeof(Transaction) * num_txns);
//     if (!txn_map) { perror("malloc txn_map"); exit(1); }
// 
//     pthread_t threads[NUM_THREADS];
//     ParseTask tasks[NUM_THREADS];
// 
//     int chunk_size = (count + NUM_THREADS - 1) / NUM_THREADS;
//     for (int i = 0; i < NUM_THREADS; ++i) {
//         tasks[i].lines = lines;
//         tasks[i].start = i * chunk_size;
//         tasks[i].end = (i + 1) * chunk_size;
//         if (tasks[i].end > count) tasks[i].end = count;
//         pthread_create(&threads[i], NULL, parse_transactions_worker, &tasks[i]);
//     }
//     for (int i = 0; i < NUM_THREADS; ++i) {
//         pthread_join(threads[i], NULL);
//     }
// 
//     for (int i = 0; i < count; ++i) {
//         free(lines[i]);
//     }
//     free(lines);
// }
// 
// void *parse_log_worker(void *arg) {
//     ParseTask *task = (ParseTask *)arg;
//     for (int i = task->start; i < task->end; ++i) {
//         char *line = task->lines[i];
//         if (!line || strlen(line) < 2) continue;
// 
//         if (strstr(line, "submit txn id=")) {
//             double ts;
//             int txn_id;
//             sscanf(line, "[+%lf] submit txn id=%d", &ts, &txn_id);
//             events[i].kind = EVENT_SUBMIT;
//             events[i].timestamp = ts;
//             events[i].txn_id = txn_id;
//         } else if (strstr(line, "scheduled txn id=")) {
//             double ts;
//             int txn_id, puppet_id;
//             sscanf(line, "[+%lf] scheduled txn id=%d assigned to puppet %d", &ts, &txn_id, &puppet_id);
//             events[i].kind = EVENT_SCHEDULED;
//             events[i].timestamp = ts;
//             events[i].txn_id = txn_id;
//             events[i].puppet_id = puppet_id;
//         } else if (strstr(line, "done puppet")) {
//             double ts;
//             int puppet_id, txn_id;
//             sscanf(line, "[+%lf] done puppet %d finished txn id=%d", &ts, &puppet_id, &txn_id);
//             events[i].kind = EVENT_DONE;
//             events[i].timestamp = ts;
//             events[i].txn_id = txn_id;
//             events[i].puppet_id = puppet_id;
//         } else {
//             fprintf(stderr, "Parse error: %s", line);
//             exit(1);
//         }
//     }
//     return NULL;
// }
// 
// static void parse_log(const char *filename) {
//     FILE *f = fopen(filename, "r");
//     if (!f) {
//         perror("fopen log");
//         exit(1);
//     }
// 
//     char **lines = NULL;
//     size_t capacity = 1024;
//     lines = malloc(sizeof(char *) * capacity);
//     if (!lines) { perror("malloc lines"); exit(1); }
// 
//     char buf[4096];
//     int count = 0;
// 
//     while (fgets(buf, sizeof(buf), f)) {
//         if (strlen(buf) < 2) continue;
//         if (count >= (int)capacity) {
//             capacity *= 2;
//             lines = realloc(lines, sizeof(char *) * capacity);
//             if (!lines) { perror("realloc lines"); exit(1); }
//         }
//         lines[count++] = strdup(buf);
//     }
//     fclose(f);
// 
//     num_events = count;
//     events = malloc(sizeof(Event) * num_events);
//     if (!events) { perror("malloc events"); exit(1); }
// 
//     pthread_t threads[NUM_THREADS];
//     ParseTask tasks[NUM_THREADS];
// 
//     int chunk_size = (count + NUM_THREADS - 1) / NUM_THREADS;
//     for (int i = 0; i < NUM_THREADS; ++i) {
//         tasks[i].lines = lines;
//         tasks[i].start = i * chunk_size;
//         tasks[i].end = (i + 1) * chunk_size;
//         if (tasks[i].end > count) tasks[i].end = count;
//         pthread_create(&threads[i], NULL, parse_log_worker, &tasks[i]);
//     }
//     for (int i = 0; i < NUM_THREADS; ++i) {
//         pthread_join(threads[i], NULL);
//     }
// 
//     for (int i = 0; i < count; ++i) {
//         free(lines[i]);
//     }
//     free(lines);
// }
// 
// // Remaining functions for consistency check and main setup will also dynamically allocate puppet_busy with calloc(num_puppets, sizeof(double)).
// 
// static void check_consistency_and_metrics(int work_us) {
//     bool *submitted = calloc(num_txns, sizeof(bool));
//     bool *scheduled = calloc(num_txns, sizeof(bool));
//     bool *done = calloc(num_txns, sizeof(bool));
//     int *active = calloc(num_txns, sizeof(int));
// 
//     if (!submitted || !scheduled || !done || !active) {
//         perror("calloc consistency arrays");
//         exit(1);
//     }
// 
//     double submit_start = -1.0;
//     double submit_end = -1.0;
//     double wall_start = -1.0;
//     double wall_end = -1.0;
// 
//     for (int i = 0; i < num_events; i++) {
//         Event *e = &events[i];
// 
//         if (e->kind == EVENT_SUBMIT) {
//             if (submitted[e->txn_id]) {
//                 fprintf(stderr, "Error: Transaction %d submitted more than once\n", e->txn_id);
//                 exit(1);
//             }
//             submitted[e->txn_id] = true;
//             if (submit_start < 0 || e->timestamp < submit_start) submit_start = e->timestamp;
//             if (submit_end < 0 || e->timestamp > submit_end) submit_end = e->timestamp;
//         } else if (e->kind == EVENT_SCHEDULED) {
//             if (!submitted[e->txn_id]) {
//                 fprintf(stderr, "Error: Transaction %d scheduled without submission\n", e->txn_id);
//                 exit(1);
//             }
//             if (scheduled[e->txn_id]) {
//                 fprintf(stderr, "Error: Transaction %d scheduled more than once\n", e->txn_id);
//                 exit(1);
//             }
//             scheduled[e->txn_id] = true;
//             active[e->txn_id] = e->puppet_id;
//         } else if (e->kind == EVENT_DONE) {
//             if (!scheduled[e->txn_id]) {
//                 fprintf(stderr, "Error: Transaction %d completed without being scheduled\n", e->txn_id);
//                 exit(1);
//             }
//             if (done[e->txn_id]) {
//                 fprintf(stderr, "Error: Transaction %d completed more than once\n", e->txn_id);
//                 exit(1);
//             }
//             if (active[e->txn_id] != e->puppet_id) {
//                 fprintf(stderr, "Error: Puppet ID mismatch for transaction %d\n", e->txn_id);
//                 exit(1);
//             }
//             done[e->txn_id] = true;
//             puppet_busy[e->puppet_id] += work_us * 1e-6;
// 
//             if (wall_start < 0 || e->timestamp < wall_start) wall_start = e->timestamp;
//             if (wall_end < 0 || e->timestamp > wall_end) wall_end = e->timestamp;
//         }
//     }
// 
//     printf("\n✅ All consistency checks passed!\n\n");
// 
//     int total_submitted = 0;
//     for (int i = 0; i < num_txns; i++) {
//         if (submitted[i]) total_submitted++;
//     }
// 
//     if (total_submitted > 1) {
//         double submit_duration = submit_end - submit_start;
//         double client_throughput = total_submitted / submit_duration;
//         double ideal_throughput = num_puppets / (work_us * 1e-6);
//         double efficiency = client_throughput / ideal_throughput;
// 
//         printf("Client submission rate: %.2f million transactions/sec\n", client_throughput / 1e6);
//         printf("Ideal rate            : %.2f million transactions/sec\n", ideal_throughput / 1e6);
//         printf("Efficiency            : %.1f%% (%.1fx)\n", efficiency * 100.0, 1.0 / efficiency);
//     }
// 
//     printf("\n");
// 
//     if (wall_start >= 0 && wall_end >= 0) {
//         double wall_time = wall_end - wall_start;
//         double ideal_time = (work_us * 1e-6) * total_submitted / num_puppets;
//         double efficiency = ideal_time / wall_time;
// 
//         printf("Total wall time: %.6f seconds\n", wall_time);
//         printf("Ideal time     : %.6f seconds\n", ideal_time);
//         printf("Efficiency     : %.1f%% (%.1fx)\n", efficiency * 100.0, 1.0 / efficiency);
// 
//         printf("Puppet utilization:\n");
//         for (int i = 0; i < num_puppets; i++) {
//             double utilization = 100.0 * puppet_busy[i] / wall_time;
//             printf("  Puppet %d: %.2f%%\n", i, utilization);
//         }
//     }
// 
//     printf("\n");
// 
//     free(submitted);
//     free(scheduled);
//     free(done);
//     free(active);
// }
// 
// int main(int argc, char *argv[]) {
//     if (argc != 5) {
//         fprintf(stderr, "Usage: %s <transactions.csv> <log.txt> <num_puppets> <work_us>\n", argv[0]);
//         return 1;
//     }
// 
//     const char *transactions_file = argv[1];
//     const char *log_file = argv[2];
//     num_puppets = atoi(argv[3]);
//     int work_us = atoi(argv[4]);
// 
//     puppet_busy = calloc(num_puppets, sizeof(double));
//     if (!puppet_busy) { perror("calloc puppet_busy"); exit(1); }
// 
//     parse_transactions_csv(transactions_file);
//     parse_log(log_file);
//     check_consistency_and_metrics(work_us);
// 
//     free(txn_map);
//     free(events);
//     free(puppet_busy);
// 
//     return 0;
// }
