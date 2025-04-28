#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>

#define MAX_OBJS_PER_TXN 32
#define NUM_THREADS 8

typedef struct {
    int reads[MAX_OBJS_PER_TXN];
    int writes[MAX_OBJS_PER_TXN];
    int num_reads;
    int num_writes;
} Transaction;

typedef enum {
    EVENT_SUBMIT,
    EVENT_SCHEDULED,
    EVENT_DONE
} EventKind;

typedef struct {
    EventKind kind;
    double timestamp;
    int txn_id;
    int puppet_id;
} Event;

Transaction *txn_map;
Event *events;
double *puppet_busy;

int num_txns = 0;
int num_events = 0;
int num_puppets = 0;

pthread_mutex_t event_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    char **lines;
    int start;
    int end;
} ParseTask;

static inline double parse_time(const char *s) {
    char *endptr;
    return strtod(s, &endptr);
}

void *parse_transactions_worker(void *arg) {
    ParseTask *task = (ParseTask *)arg;
    for (int i = task->start; i < task->end; ++i) {
        char *line = task->lines[i];
        if (!line || strlen(line) < 2) continue;

        Transaction *txn = &txn_map[i];
        txn->num_reads = 0;
        txn->num_writes = 0;

        char *p = strtok(line, ",");
        if (!p) continue;

        while ((p = strtok(NULL, ","))) {
            int objid = atoi(p);
            p = strtok(NULL, ",");
            if (!p) break;
            int writeflag = atoi(p);

            if (writeflag) {
                txn->writes[txn->num_writes++] = objid;
            } else {
                txn->reads[txn->num_reads++] = objid;
            }
        }
    }
    return NULL;
}

static void parse_transactions_csv(const char *filename) {
    FILE *f = fopen(filename, "r");
    if (!f) {
        perror("fopen transactions");
        exit(1);
    }

    char **lines = NULL;
    size_t capacity = 1024;
    lines = malloc(sizeof(char *) * capacity);
    if (!lines) { perror("malloc lines"); exit(1); }

    char buf[4096];
    int count = 0;

    while (fgets(buf, sizeof(buf), f)) {
        if (strlen(buf) < 2) continue;
        if (count >= (int)capacity) {
            capacity *= 2;
            lines = realloc(lines, sizeof(char *) * capacity);
            if (!lines) { perror("realloc lines"); exit(1); }
        }
        lines[count++] = strdup(buf);
    }
    fclose(f);

    num_txns = count;
    txn_map = malloc(sizeof(Transaction) * num_txns);
    if (!txn_map) { perror("malloc txn_map"); exit(1); }

    pthread_t threads[NUM_THREADS];
    ParseTask tasks[NUM_THREADS];

    int chunk_size = (count + NUM_THREADS - 1) / NUM_THREADS;
    for (int i = 0; i < NUM_THREADS; ++i) {
        tasks[i].lines = lines;
        tasks[i].start = i * chunk_size;
        tasks[i].end = (i + 1) * chunk_size;
        if (tasks[i].end > count) tasks[i].end = count;
        pthread_create(&threads[i], NULL, parse_transactions_worker, &tasks[i]);
    }
    for (int i = 0; i < NUM_THREADS; ++i) {
        pthread_join(threads[i], NULL);
    }

    for (int i = 0; i < count; ++i) {
        free(lines[i]);
    }
    free(lines);
}

void *parse_log_worker(void *arg) {
    ParseTask *task = (ParseTask *)arg;
    for (int i = task->start; i < task->end; ++i) {
        char *line = task->lines[i];
        if (!line || strlen(line) < 2) continue;

        if (strstr(line, "submit txn id=")) {
            double ts;
            int txn_id;
            sscanf(line, "[+%lf] submit txn id=%d", &ts, &txn_id);
            events[i].kind = EVENT_SUBMIT;
            events[i].timestamp = ts;
            events[i].txn_id = txn_id;
        } else if (strstr(line, "scheduled txn id=")) {
            double ts;
            int txn_id, puppet_id;
            sscanf(line, "[+%lf] scheduled txn id=%d assigned to puppet %d", &ts, &txn_id, &puppet_id);
            events[i].kind = EVENT_SCHEDULED;
            events[i].timestamp = ts;
            events[i].txn_id = txn_id;
            events[i].puppet_id = puppet_id;
        } else if (strstr(line, "done puppet")) {
            double ts;
            int puppet_id, txn_id;
            sscanf(line, "[+%lf] done puppet %d finished txn id=%d", &ts, &puppet_id, &txn_id);
            events[i].kind = EVENT_DONE;
            events[i].timestamp = ts;
            events[i].txn_id = txn_id;
            events[i].puppet_id = puppet_id;
        } else {
            fprintf(stderr, "Parse error: %s", line);
            exit(1);
        }
    }
    return NULL;
}

static void parse_log(const char *filename) {
    FILE *f = fopen(filename, "r");
    if (!f) {
        perror("fopen log");
        exit(1);
    }

    char **lines = NULL;
    size_t capacity = 1024;
    lines = malloc(sizeof(char *) * capacity);
    if (!lines) { perror("malloc lines"); exit(1); }

    char buf[4096];
    int count = 0;

    while (fgets(buf, sizeof(buf), f)) {
        if (strlen(buf) < 2) continue;
        if (count >= (int)capacity) {
            capacity *= 2;
            lines = realloc(lines, sizeof(char *) * capacity);
            if (!lines) { perror("realloc lines"); exit(1); }
        }
        lines[count++] = strdup(buf);
    }
    fclose(f);

    num_events = count;
    events = malloc(sizeof(Event) * num_events);
    if (!events) { perror("malloc events"); exit(1); }

    pthread_t threads[NUM_THREADS];
    ParseTask tasks[NUM_THREADS];

    int chunk_size = (count + NUM_THREADS - 1) / NUM_THREADS;
    for (int i = 0; i < NUM_THREADS; ++i) {
        tasks[i].lines = lines;
        tasks[i].start = i * chunk_size;
        tasks[i].end = (i + 1) * chunk_size;
        if (tasks[i].end > count) tasks[i].end = count;
        pthread_create(&threads[i], NULL, parse_log_worker, &tasks[i]);
    }
    for (int i = 0; i < NUM_THREADS; ++i) {
        pthread_join(threads[i], NULL);
    }

    for (int i = 0; i < count; ++i) {
        free(lines[i]);
    }
    free(lines);
}

// Remaining functions for consistency check and main setup will also dynamically allocate puppet_busy with calloc(num_puppets, sizeof(double)).

static void check_consistency_and_metrics(int work_us) {
    bool *submitted = calloc(num_txns, sizeof(bool));
    bool *scheduled = calloc(num_txns, sizeof(bool));
    bool *done = calloc(num_txns, sizeof(bool));
    int *active = calloc(num_txns, sizeof(int));

    if (!submitted || !scheduled || !done || !active) {
        perror("calloc consistency arrays");
        exit(1);
    }

    double submit_start = -1.0;
    double submit_end = -1.0;
    double wall_start = -1.0;
    double wall_end = -1.0;

    for (int i = 0; i < num_events; i++) {
        Event *e = &events[i];

        if (e->kind == EVENT_SUBMIT) {
            if (submitted[e->txn_id]) {
                fprintf(stderr, "Error: Transaction %d submitted more than once\n", e->txn_id);
                exit(1);
            }
            submitted[e->txn_id] = true;
            if (submit_start < 0 || e->timestamp < submit_start) submit_start = e->timestamp;
            if (submit_end < 0 || e->timestamp > submit_end) submit_end = e->timestamp;
        } else if (e->kind == EVENT_SCHEDULED) {
            if (!submitted[e->txn_id]) {
                fprintf(stderr, "Error: Transaction %d scheduled without submission\n", e->txn_id);
                exit(1);
            }
            if (scheduled[e->txn_id]) {
                fprintf(stderr, "Error: Transaction %d scheduled more than once\n", e->txn_id);
                exit(1);
            }
            scheduled[e->txn_id] = true;
            active[e->txn_id] = e->puppet_id;
        } else if (e->kind == EVENT_DONE) {
            if (!scheduled[e->txn_id]) {
                fprintf(stderr, "Error: Transaction %d completed without being scheduled\n", e->txn_id);
                exit(1);
            }
            if (done[e->txn_id]) {
                fprintf(stderr, "Error: Transaction %d completed more than once\n", e->txn_id);
                exit(1);
            }
            if (active[e->txn_id] != e->puppet_id) {
                fprintf(stderr, "Error: Puppet ID mismatch for transaction %d\n", e->txn_id);
                exit(1);
            }
            done[e->txn_id] = true;
            puppet_busy[e->puppet_id] += work_us * 1e-6;

            if (wall_start < 0 || e->timestamp < wall_start) wall_start = e->timestamp;
            if (wall_end < 0 || e->timestamp > wall_end) wall_end = e->timestamp;
        }
    }

    printf("\n✅ All consistency checks passed!\n\n");

    int total_submitted = 0;
    for (int i = 0; i < num_txns; i++) {
        if (submitted[i]) total_submitted++;
    }

    if (total_submitted > 1) {
        double submit_duration = submit_end - submit_start;
        double client_throughput = total_submitted / submit_duration;
        double ideal_throughput = num_puppets / (work_us * 1e-6);
        double efficiency = client_throughput / ideal_throughput;

        printf("Client submission rate: %.2f million transactions/sec\n", client_throughput / 1e6);
        printf("Ideal rate            : %.2f million transactions/sec\n", ideal_throughput / 1e6);
        printf("Efficiency            : %.1f%% (%.1fx)\n", efficiency * 100.0, 1.0 / efficiency);
    }

    printf("\n");

    if (wall_start >= 0 && wall_end >= 0) {
        double wall_time = wall_end - wall_start;
        double ideal_time = (work_us * 1e-6) * total_submitted / num_puppets;
        double efficiency = ideal_time / wall_time;

        printf("Total wall time: %.6f seconds\n", wall_time);
        printf("Ideal time     : %.6f seconds\n", ideal_time);
        printf("Efficiency     : %.1f%% (%.1fx)\n", efficiency * 100.0, 1.0 / efficiency);

        printf("Puppet utilization:\n");
        for (int i = 0; i < num_puppets; i++) {
            double utilization = 100.0 * puppet_busy[i] / wall_time;
            printf("  Puppet %d: %.2f%%\n", i, utilization);
        }
    }

    printf("\n");

    free(submitted);
    free(scheduled);
    free(done);
    free(active);
}

int main(int argc, char *argv[]) {
    if (argc != 5) {
        fprintf(stderr, "Usage: %s <transactions.csv> <log.txt> <num_puppets> <work_us>\n", argv[0]);
        return 1;
    }

    const char *transactions_file = argv[1];
    const char *log_file = argv[2];
    num_puppets = atoi(argv[3]);
    int work_us = atoi(argv[4]);

    puppet_busy = calloc(num_puppets, sizeof(double));
    if (!puppet_busy) { perror("calloc puppet_busy"); exit(1); }

    parse_transactions_csv(transactions_file);
    parse_log(log_file);
    check_consistency_and_metrics(work_us);

    free(txn_map);
    free(events);
    free(puppet_busy);

    return 0;
}
