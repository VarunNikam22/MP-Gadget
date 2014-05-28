#include <stdlib.h>
#include <mpi.h>
#include <string.h>
#include <stdio.h>
#include "walltime.h"

static struct ClockTable * CT = NULL;

static double WallTimeClock;
static double LastReportTime;

static void walltime_clock_insert(char * name);
static void walltime_summary_clocks(struct Clock * C, int N);
static void walltime_update_parents();
static double seconds();

void walltime_init(struct ClockTable * ct) {
    CT = ct;
    CT->Nmax = 128;
    CT->N = 0;
    CT->ElapsedTime = 0;
    walltime_reset();
    walltime_clock_insert("/");
    LastReportTime = seconds();
}

static void walltime_summary_clocks(struct Clock * C, int N) {
    double t[N];
    double min[N];
    double max[N];
    double sum[N];
    int i;
    for(i = 0; i < CT->N; i ++) {
        t[i] = C[i].time;
    }
    MPI_Reduce(t, min, N, MPI_DOUBLE, MPI_MIN, 0, MPI_COMM_WORLD);
    MPI_Reduce(t, max, N, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
    MPI_Reduce(t, sum, N, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);

    int NTask;
    MPI_Comm_size(MPI_COMM_WORLD, &NTask);
    /* min, max and mean are good only on process 0 */
    for(i = 0; i < CT->N; i ++) {
        C[i].min = min[i];
        C[i].max = max[i];
        C[i].mean = sum[i] / NTask;
    }
}

/* put min max mean of MPI ranks to rank 0*/
/* AC will have the total timing, C will have the current step information */
void walltime_summary() {
    walltime_update_parents();
    int i;
    int N = 0;
    int rank = 0;
    /* add to the cumulative time */
    for(i = 0; i < CT->N; i ++) {
        CT->AC[i].time += CT->C[i].time;
    }
    walltime_summary_clocks(CT->C, CT->N);
    walltime_summary_clocks(CT->AC, CT->N);

    /* clear .time for next step */
    for(i = 0; i < CT->N; i ++) {
        CT->C[i].time = 0;
    }
    /* wo do this here because all processes are sync after summary_clocks*/
    double step_all = seconds() - LastReportTime;
    printf("step_all = %g\n", step_all);
    LastReportTime = seconds();
    CT->ElapsedTime += step_all;
    CT->StepTime = step_all;
}

static int clockcmp(const void * c1, const void * c2) {
    const struct Clock * p1 = c1;
    const struct Clock * p2 = c2;
    return strcmp(p1->name, p2->name);
}

static void walltime_clock_insert(char * name) {
    if(name[0] != '/') abort();
    if(strlen(name) > 1) {
        char tmp[80];
        strcpy(tmp, name);
        char * p;
        int parent = walltime_clock("/");
        for(p = tmp + 1; *p; p ++) {
            if (*p == '/') {
                *p = 0;
                int parent = walltime_clock(tmp);
                *p = '/';
            }
        }
    }
    if(CT->N == CT->Nmax) {
        /* too many counters */
        abort();
    }
    strcpy(CT->C[CT->N].name, name);
    strcpy(CT->AC[CT->N].name, CT->C[CT->N].name);
    CT->N ++;
    qsort(CT->C, CT->N, sizeof(struct Clock), clockcmp);
    qsort(CT->AC, CT->N, sizeof(struct Clock), clockcmp);
}

int walltime_clock(char * name) {
    struct Clock dummy;
    strcpy(dummy.name, name);
    struct Clock * rt = bsearch(&dummy, CT->C, CT->N, sizeof(struct Clock), clockcmp);
    if(rt == NULL) {
        walltime_clock_insert(name);
        rt = bsearch(&dummy, CT->C, CT->N, sizeof(struct Clock), clockcmp);
    }
    return rt - CT->C;
};

char walltime_get_symbol(char * name) {
    int id = walltime_clock(name);
    return CT->C[id].symbol;
}

double walltime_get(char * name, enum clocktype type) {
    int id = walltime_clock(name);
    /* only make sense on root */
    switch(type) {
        case CLOCK_STEP_MEAN:
            return CT->C[id].mean;
        case CLOCK_STEP_MIN:
            return CT->C[id].min;
        case CLOCK_STEP_MAX:
            return CT->C[id].max;
        case CLOCK_ACCU_MEAN:
            return CT->AC[id].mean;
        case CLOCK_ACCU_MIN:
            return CT->AC[id].min;
        case CLOCK_ACCU_MAX:
            return CT->AC[id].max;
    }
    return 0;
}
double walltime_get_time(char * name) {
    int id = walltime_clock(name);
    return CT->C[id].time;
}

static void walltime_update_parents() {
    /* returns the sum of every clock with the same prefix */
    int i = 0;
    for(i = 0; i < CT->N; i ++) {
        int j;
        char * prefix = CT->C[i].name;
        int l = strlen(prefix);
        double t = 0;
        for(j = i + 1; j < CT->N; j++) {
            if(0 == strncmp(prefix, CT->C[j].name, l)) {
                t += CT->C[j].time;
            } else {
                break;
            }
        }
        /* update only if there are children */
        if (t > 0) CT->C[i].time = t;
    }
}

void walltime_reset() {
    WallTimeClock = seconds();
}

double walltime_add(char * name, double dt) {
    int id = walltime_clock(name);
    CT->C[id].time += dt;
    return dt;
}
double walltime_measure(char * name) {
    double t = seconds();
    double dt = t - WallTimeClock;
    WallTimeClock = seconds();
    if(name != NULL) {
        int id = walltime_clock(name);
        CT->C[id].time += dt;
    }
    return dt;
}

/* returns the number of cpu-ticks in seconds that
 * have elapsed. (or the wall-clock time)
 */
static double seconds(void)
{
#ifdef WALLCLOCK
  return MPI_Wtime();
#else
  return ((double) clock()) / CLOCKS_PER_SEC;
#endif

  /* note: on AIX and presumably many other 32bit systems, 
   * clock() has only a resolution of 10ms=0.01sec 
   */
}
void walltime_report(FILE * fp) {
    int i; 
    for(i = 0; i < CT->N; i ++) {
        char * name = CT->C[i].name;
        fprintf(fp, "%-26s  %10.2f %4.1f%%  %10.2f %4.1f%%  %10.2f %10.2f\n",
                CT->C[i].name,
                CT->AC[i].mean,
                CT->AC[i].mean / CT->ElapsedTime * 100.,
                CT->C[i].mean,
                CT->C[i].mean / CT->StepTime * 100.,
                CT->C[i].min,
                CT->C[i].max
                );
    }
}
#if 0
#define HELLO atom(&atomtable, "Hello")
#define WORLD atom(&atomtable, "WORLD")
int main() {
    walltime_init();
    printf("%d\n", HELLO, atom_name(&atomtable, HELLO));
    printf("%d\n", HELLO, atom_name(&atomtable, HELLO));
    printf("%d\n", WORLD, atom_name(&atomtable, WORLD));
    printf("%d\n", HELLO, atom_name(&atomtable, HELLO));
}
#endif
