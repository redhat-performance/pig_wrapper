/*
 *
 * pig - Workload generator / simulator
 * Copyright (C) 2012 Bill Gray (bgray@redhat.com), Red Hat Inc
 *
 * pig is free software; you can redistribute it and/or modify it under the
 * terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation; version 2.1.
 *
 * pig is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 * PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more
 * details.
 *
 * You should find a copy of v2.1 of the GNU Lesser General Public License
 * somewhere on your Linux system; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 *
 * */ 


//
// Compile with: gcc -std=gnu99 -g -Wall -pthread -o pig pig.c /usr/lib64/libnuma.so.1 -lm
//


#define _GNU_SOURCE
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <linux/mempolicy.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <values.h>


extern int numa_max_node(void);
extern int set_mempolicy(int mode, unsigned long *nodemask, unsigned long maxnode); 
extern int mbind(void *addr, unsigned long len, int mode, unsigned long *nodemask, unsigned long maxnode, unsigned flags);


#define MAX_CPUS 2048
#define MAX_NODES 64
#define MEM_STRIDE 57
#define DIRTY_RATE 100
#define ALARM_INTERVAL 1
#define SPIN_LIMIT 10000
#define MEGABYTE ((uint64_t)1024 * 1024)
#define GIGABYTE ((uint64_t)1024 * 1024 * 1024)

#ifndef MAP_HUGETLB
/* Create huge page mapping. */
#define MAP_HUGETLB 0x40000
#endif

#ifndef MADV_HUGEPAGE
/* Worth backing with hugepages. */
#define MADV_HUGEPAGE 14
#endif

#ifndef MADV_NOHUGEPAGE
/* Not worth backing with hugepages.  */
#define MADV_NOHUGEPAGE 15
#endif


#define CHECK_ERR(rc, msg) \
    if (rc != 0) { \
        errno = rc; \
	perror(msg); \
	exit(EXIT_FAILURE); \
    }


#if defined(__GNUC__) && defined(__aarch64__) 


static __inline__ uint64_t start_clock() {
    uint64_t ticks;
    __asm__ __volatile__ ("isb; mrs %0, cntvct_el0" : "=r" (ticks));
    return ticks;
}

static __inline__ uint64_t stop_clock() {
    uint64_t ticks;
    __asm__ __volatile__ ("isb; mrs %0, cntvct_el0" : "=r" (ticks));
    return ticks;
}


#elif defined(__GNUC__) && defined(__arm__)


static __inline__ uint64_t start_clock() {
    uint32_t hi, lo;
    asm volatile("mrrc p15, 0, %0, %1, c9" : "=r"(hi), "=r"(lo));
    return ( (uint64_t)lo) | ( ((uint64_t)hi) << 32);
}

static __inline__ uint64_t stop_clock() {
    uint32_t hi, lo;
    asm volatile("mrrc p15, 0, %0, %1, c9" : "=r"(hi), "=r"(lo));
    return ( (uint64_t)lo) | ( ((uint64_t)hi) << 32);
}


#elif defined(__GNUC__) && defined (__x86_64__)


static __inline__ int64_t rdtsc(void)
{
    unsigned a, d;
    asm volatile("rdtsc" : "=a" (a), "=d" (d));
    return ((unsigned long)a) | (((unsigned long)d) << 32);
}

static __inline__ int64_t rdtsc_s(void)
{
    unsigned a, d;
    asm volatile("cpuid" ::: "%rax", "%rbx", "%rcx", "%rdx");
    asm volatile("rdtsc" : "=a" (a), "=d" (d));
    return ((unsigned long)a) | (((unsigned long)d) << 32);
}

static __inline__ int64_t rdtsc_e(void)
{
    unsigned a, d;
    asm volatile("rdtscp" : "=a" (a), "=d" (d));
    asm volatile("cpuid" ::: "%rax", "%rbx", "%rcx", "%rdx");
    return ((unsigned long)a) | (((unsigned long)d) << 32);
}


static __inline__ uint64_t start_clock() {
    // See: Intel Doc #324264, "How to Benchmark Code Execution Times on Intel...",
    uint32_t hi, lo;
    __asm__ __volatile__ (
        "CPUID\n\t"
        "RDTSC\n\t"
        "mov %%edx, %0\n\t"
        "mov %%eax, %1\n\t": "=r" (hi), "=r" (lo)::
        "%rax", "%rbx", "%rcx", "%rdx");
    return ( (uint64_t)lo) | ( ((uint64_t)hi) << 32);
}

static __inline__ uint64_t stop_clock() {
    // See: Intel Doc #324264, "How to Benchmark Code Execution Times on Intel...",
    uint32_t hi, lo;
    __asm__ __volatile__(
        "RDTSCP\n\t"
        "mov %%edx, %0\n\t"
        "mov %%eax, %1\n\t"
        "CPUID\n\t": "=r" (hi), "=r" (lo)::
        "%rax", "%rbx", "%rcx", "%rdx");
    return ( (uint64_t)lo) | ( ((uint64_t)hi) << 32);
}


#elif defined (foo__PPC64__)


static __inline__ uint64_t start_clock() {
    return __ppc_get_timebase();
}

static __inline__ uint64_t stop_clock() {
    return __ppc_get_timebase();
}


#else


static __inline__ uint64_t start_clock() {
    struct timespec tspec;
    int rc = clock_gettime(CLOCK_MONOTONIC, &tspec);
    if (rc < 0) {
        return 0;
    } else {
        return (tspec.tv_sec * 1000000000) + tspec.tv_nsec;
    }
}

static __inline__ uint64_t stop_clock() {
    struct timespec tspec;
    int rc = clock_gettime(CLOCK_MONOTONIC, &tspec);
    if (rc < 0) {
        return 0;
    } else {
        return (tspec.tv_sec * 1000000000) + tspec.tv_nsec;
    }
}


#endif


int debug = 0;
int use_global_mem = 0;
int use_interleaved_mem = 0;
int quiet = 0;
int verbose = 0;
int num_cpus = 0;
int num_nodes = 0;
int round_robin = 0;
int processes = 1;
int threads = 1;
int megabytes = 0;
int min_pct = 100;
int max_pct = 100;
int seconds = 0;
int rest_seconds = 0;
int work_seconds = 0;
int save_work_seconds = 0;
int use_THP_mem    = 0;
int avoid_THP_mem  = 0;
int use_huge_pages = 0;
int use_shared_mem = 0;
int use_mergeable_mem = 0;
int also_write_mem = 0;
int dirty_rate = DIRTY_RATE;
int mem_stride = MEM_STRIDE;
int do_fpu_load  = 0;
int do_int_load  = 0;
int do_mem_load  = 0;
int do_dirty_load  = 0;
int do_spin_load = 1;
int do_sleep_load = 0;
uint64_t *mem = NULL;
sem_t *sem_mutex_ptr;

typedef struct node_data {
    int node_ix;
    cpu_set_t *cpu_set_p;
    size_t cpu_set_size;
} node_data_t, *node_data_p;

node_data_t node[MAX_NODES];


typedef struct thread_data {
    uint64_t cpu[MAX_CPUS];
    uint64_t *mem;
    uint64_t work_done;
    pthread_t tid;
    pid_t pid;
    uint32_t tix;
    uint32_t flag_quit;
    uint32_t flag_sleep;
    uint32_t flag_check_cpu;
} thread_data_t, *thread_data_p;

thread_data_p thread = NULL;



void print_usage_and_exit(char *prog_name) {
    fprintf(stderr, "USAGE: %s <options> ...\n", prog_name);
    fprintf(stderr, "BEHAVIOR OPTIONS\n");
    // fprintf(stderr, "-d for debug\n");
    fprintf(stderr, "-h to print this usage info\n");
    fprintf(stderr, "-i <WORK[:REST]> set intermittent load seconds\n");
    fprintf(stderr, "-k <MIN[:MAX]> set min and optional max percent spin load thread CPU utilization\n");
    fprintf(stderr, "-l <LOAD>, one of { 'spin', 'sleep', 'int', 'fpu', 'mem', 'dirty' }\n");
    fprintf(stderr, "   The spin load is the default, and the only load with variable percent\n");
    fprintf(stderr, "-m <N> to allocate and initialize <N> MBs of memory per thread\n");
    fprintf(stderr, "   also use the -G option if you want to make it <N> MBs per process\n");
    fprintf(stderr, "-n <N> for dirty load to write <N> random memory locations per second\n");
    fprintf(stderr, "-p <N> to fork <N> processes\n");
    // fprintf(stderr, "-q for quiet\n");
    fprintf(stderr, "-t <N> to create <N> threads in each process\n");
    fprintf(stderr, "-s <N> to run for <N> seconds before quitting\n");
    fprintf(stderr, "-r to round robin bind processes to nodes\n");
    fprintf(stderr, "-v for verbose\n");
    fprintf(stderr, "-V to show version info\n");
    fprintf(stderr, "-w for mem load to also write memory.  Default is pointer-chasing reads\n");
    fprintf(stderr, "MEMORY ATTRIBUTES\n");
    fprintf(stderr, "-G for global per process memory (instead of per thread memory)\n");
    fprintf(stderr, "-I to use interleaved memory\n");
    fprintf(stderr, "-H to use static huge page memory.\n");
    fprintf(stderr, "-M to use mergeable memory.\n");
    fprintf(stderr, "-T to use transparent huge page memory.\n");
    fprintf(stderr, "-N to NOT use transparent huge page memory.\n");
    fprintf(stderr, "-S to use shared anon memory. Default is private anon memory\n");
    fprintf(stderr, "   Shared memory is not compatible with mergeable or THP memory\n");
    fprintf(stderr, "-L <N> to set mem load stride length.  Default is 57 64-bit words\n");
    exit(EXIT_FAILURE);
}


void print_version_and_exit(char *prog_name) {
    char *version_string = "20151011";
    fprintf(stdout, "%s version: %s: %s\n", prog_name, version_string, __DATE__);
    exit(EXIT_SUCCESS);
}


void dump_thread_cpu_histograms() {
    int col_has_data[MAX_CPUS];
    memset(col_has_data, 0, sizeof(col_has_data));
    for (int ix = 0;  (ix < threads);  ix++) {
        for (int iy = 0;  (iy < MAX_CPUS);  iy++) {
            col_has_data[iy] |= thread[ix].cpu[iy]; 
        }
    }
    printf("\nTally of samples running on each cpu:\n");
    // Start CPU id line
    if (processes > 1) {
        printf("%7d", getpid());
    }
    printf("     ");
    for (int iy = 0;  (iy < MAX_CPUS);  iy++) {
        if (col_has_data[iy]) {
            printf("%4d", iy);
        }
    }
    printf("\n");
    // Start dash line
    if (processes > 1) {
        printf("%7d", getpid());
    }
    printf("     ");
    for (int iy = 0;  (iy < MAX_CPUS);  iy++) {
        if (col_has_data[iy]) {
            printf(" ---");
        }
    }
    printf("\n");
    // Start thread lines
    for (int ix = 0;  (ix < threads);  ix++) {
        if (processes > 1) {
            printf("%7d", getpid());
        }
        printf(" %03d:", ix);
        for (int iy = 0;  (iy < MAX_CPUS);  iy++) {
            if (col_has_data[iy]) {
                if (thread[ix].cpu[iy]) {
                    printf("%4ld", thread[ix].cpu[iy]);
                } else {
                    printf("   .");
                }
            }
        }
        printf("\n");
    }
    fflush(stdout);
}


void print_avg_thread_work_done() {
    long max = 0;
    long min = MAXLONG;
    double sum = 0.0;
    if (verbose) {
        // printf("Work done on each thread:\n");
    }
    int cpus = 0;
    for (int ix = 0;  (ix < threads);  ix++) {
        if (verbose) {
            // printf("%3d: %8ld\n", ix, thread[ix].work_done);
        }
        for (int iy = 0;  (iy < MAX_CPUS);  iy++) {
            cpus += (thread[ix].cpu[iy] != 0); 
        }
        if (min > thread[ix].work_done) {
            min = thread[ix].work_done;
        }
        if (max < thread[ix].work_done) {
            max = thread[ix].work_done;
        }
        sum += (double)thread[ix].work_done;
    }
    if (threads == 1) {
        printf("PID: %d  #CPUS: %d  Threads: %d  Work: %ld\n", thread[0].pid, cpus, threads, max);
    } else {
        double avg = sum / threads;
        sum = 0.0;
        for (int ix = 0;  (ix < threads);  ix++) {
            double diff = (double)thread[ix].work_done - avg;
            sum += (diff * diff);
        }
        double variance = sum / threads;
        double stddev = sqrt(variance);
        printf("PID: %d  #CPUS: %d  Threads: %d  Min: %ld  Max: %ld  Avg: %5.1lf  Stddev: %5.1lf\n",
            thread[0].pid, cpus, threads, min, max, avg, stddev);
    }
    fflush(stdout);
}


void dump_thread_data() {
    sem_wait(sem_mutex_ptr);
    if (verbose) {
        dump_thread_cpu_histograms();
    }
    print_avg_thread_work_done();
    sem_post(sem_mutex_ptr);
}


void sigint_handler(int sig) {
    for (int ix = 0;  (ix < threads);  ix++) {
        thread[ix].flag_quit = 1;
    }
}


void sigalrm_handler(int sig) {
    int process_going_to_sleep = 0;
    for (int ix = 0;  (ix < threads);  ix++) {
        thread[ix].flag_check_cpu = 1;
    }
    if (work_seconds > 0) {
        work_seconds -= ALARM_INTERVAL;
        if (work_seconds <= 0) {
            work_seconds = save_work_seconds;
	    process_going_to_sleep = 1;
            for (int ix = 0;  (ix < threads);  ix++) {
                thread[ix].flag_sleep = 1;
            }
        }
    }
    if (seconds > 0) {
        seconds -= ALARM_INTERVAL;
        if (seconds <= 0) {
            for (int ix = 0;  (ix < threads);  ix++) {
                thread[ix].flag_quit = 1;
            }
            return;
        }
        if (rest_seconds > seconds) {
    	    rest_seconds = seconds;
	}
	if (process_going_to_sleep) {
	    seconds -= rest_seconds;
	    if (seconds < 1) {
	        seconds = 1;
	    }
	}
    }
    if (process_going_to_sleep) {
	alarm(rest_seconds + ALARM_INTERVAL);
    } else {
	alarm(ALARM_INTERVAL);
    }
}


void bind_to_cpu(int cpu_id, size_t cpu_set_size, cpu_set_t *cpu_set_p) {
    CPU_ZERO_S(cpu_set_size, cpu_set_p);
    CPU_SET_S(cpu_id, cpu_set_size, cpu_set_p);
    if (sched_setaffinity(0, cpu_set_size, cpu_set_p) < 0) {
        perror("sched_setaffinity");
        exit(EXIT_FAILURE);
    }
    sched_yield();
}


int get_nodes() {
    int num_nodes = 0;
    memset(node, 0, MAX_NODES * sizeof(node_data_t));
    for (;;) {
#define BUF_SIZE 128
        char fname[BUF_SIZE];
        snprintf(fname, sizeof(fname), "/sys/devices/system/node/node%d/cpulist", num_nodes);
        FILE *fd = fopen(fname, "r");
        if (!fd) {
            break;  // Assume only fopen() failure will be end of nodes....
        } else {
            if (num_nodes >= MAX_NODES) {
                printf("static NODE table too small\n");
                exit(EXIT_FAILURE);
            }
            // initialize node_data_t structure for this node
            node[num_nodes].node_ix = num_nodes;
            node[num_nodes].cpu_set_p = CPU_ALLOC(num_cpus);
            if (node[num_nodes].cpu_set_p == NULL) {
                perror("CPU_ALLOC");
                exit(EXIT_FAILURE);
            }
            node[num_nodes].cpu_set_size = CPU_ALLOC_SIZE(num_cpus);
            CPU_ZERO_S(node[num_nodes].cpu_set_size, node[num_nodes].cpu_set_p);
            // read lines from node file, and set corresponding bits in node cpu set
            char line_buf[BUF_SIZE];
get_next_line:
            while (fgets(line_buf, BUF_SIZE, fd)) {
                int in_range = 0;
                int next_cpu = 0;
                char *p = line_buf;
                for (;;) {
                    // skip over non-digits
                    while (!isdigit(*p)) {
                        if (*p == '\0') {
                            goto get_next_line;
                        }
                        if (*p++ == '-') {
                            in_range = 1;
                        }
                    }
                    // convert consecutive digits to a cpu number
                    int cpu = *p++ - '0';
                    while (isdigit(*p)) {
                        cpu *= 10;
                        cpu += (*p++ - '0');
                    }
                    if (!in_range) {
                        next_cpu = cpu;
                    }
                    for (;  (next_cpu <= cpu);  next_cpu++) {
                        CPU_SET_S(next_cpu, node[num_nodes].cpu_set_size, node[num_nodes].cpu_set_p);
                    }
                    in_range = 0;
                }

            }
            num_nodes += 1;
        }
    }
    return num_nodes;
}


void free_mem(int megabytes) {
    size_t mem_size = megabytes * MEGABYTE;
    if (munmap(mem, mem_size) < 0) {
	perror("munmap");
	exit(EXIT_FAILURE);
    }
}


char *get_mem(int megabytes) {
    unsigned long node_mask[] = { -1L, -1L, -1L, -1L, -1L, -1L };
    // FIXME: numa_max_node() seems to return the right number, but it does not
    // seem enough for the set_mem routines.  BZ this.  Seems like a bug.
    int max_node = numa_max_node() + 2;
    if (use_interleaved_mem) {
        // Set the mempolicy to interleave
        // Doing this because mbind() seems unreliable...
        if (set_mempolicy(MPOL_INTERLEAVE, node_mask, max_node) < 0) {
            perror("set_mempolicy");
            exit(EXIT_FAILURE);
        }
    }
    // Allocate the memory
    int flags = MAP_ANONYMOUS;
    if (use_shared_mem) {
	flags |= MAP_SHARED;
    } else {
	flags |= MAP_PRIVATE;
    }
    if (use_huge_pages) {
	flags |= MAP_HUGETLB;
    }
    size_t mem_size = megabytes * MEGABYTE;
    char *mem = mmap(NULL, mem_size, PROT_READ | PROT_WRITE, flags, 0, 0);
    if (mem == MAP_FAILED) {
	perror("mmap");
	exit(EXIT_FAILURE);
    }
    if (use_mergeable_mem) {
        if (madvise(mem, mem_size, MADV_MERGEABLE) < 0) {
            perror("madvise MERGEABLE");
            exit(EXIT_FAILURE);
        }
    }
    if (use_THP_mem) {
        if (madvise(mem, mem_size, MADV_HUGEPAGE) < 0) {
            perror("madvise HUGEPAGE");
            exit(EXIT_FAILURE);
        }
    }
    if (avoid_THP_mem) {
        if (madvise(mem, mem_size, MADV_NOHUGEPAGE) < 0) {
            perror("madvise NOHUGEPAGE");
            exit(EXIT_FAILURE);
        }
    }

#if 0
    // FIXME: BZ this.  It seems broken, which is why using set_mempolicy instead...
    if (use_interleaved_mem) {      
        // Try to make it interleaved...
        if (mbind(mem, mem_size, MPOL_INTERLEAVE, node_mask, max_node, 0) < 0) {
            perror("mbind");
            exit(EXIT_FAILURE);
        }
    } 
#endif

    // Initialize the newly allocated memory
    if (!do_mem_load) {
	// zero all the memory
        memset(mem, 0, mem_size);
    } else {
	// make pointer chain through the memory
	uint64_t *ptr = (uint64_t *)mem;
	uint64_t num_words = mem_size / sizeof(uint64_t *);
	while (ptr < (uint64_t *)mem + num_words - (mem_stride + 1)) {
	    *ptr = (uint64_t)(ptr + mem_stride);
	    ptr = (uint64_t *)*ptr;
	}
	*ptr = (uint64_t)NULL;
    }
    return mem;
}


#define CHECK_FLAGS { \
    if (thread[tix].flag_quit) { \
        return; \
    } \
    if (thread[tix].flag_sleep) { \
        thread[tix].flag_sleep = 0; \
	sleep(rest_seconds); \
    } \
    if (thread[tix].flag_check_cpu) { \
        thread[tix].flag_check_cpu = 0; \
        thread[tix].cpu[sched_getcpu()] += 1; \
    } \
}


void fpu_load(int tix) {
    for (;;) {
	for (int ix = 0;  (ix < (SPIN_LIMIT * 10));  ix++) {
            volatile double d = 1.0;
            // Make compiler generate mul, div, and actually call sin()
            d *= (double)(tix | 0x1);
            d /= (double)(tix | 0x1);
            d = sin(d);
            while (fabs(d - 1.0) > 1.0e-15) {
                d = sqrt(d);
            }
        }
        CHECK_FLAGS;
        thread[tix].work_done += 1;
        sched_yield(); 
    }
}


void int_load(int tix) {
    for (;;) {
	volatile uint64_t num = 0;
	for (int ix = 0;  (ix < (SPIN_LIMIT * 100));  ix++) {
            if ((num & ~(uint64_t)0xff) == 0) {
                num = start_clock();
            }
            num /= (ix | 0x1);
            num *= num;
            num /= (ix | 0x1);
            num *= num;
            num += ix;
        }
        CHECK_FLAGS;
        thread[tix].work_done += 1;
        sched_yield(); 
    }
}


void mem_load(int tix) {
    for (;;) {
        uint64_t *ptr = thread[tix].mem;
        if (also_write_mem) {
            while (*ptr) {
                *(ptr + 1) = thread[tix].work_done;
                ptr = (uint64_t *)*ptr;
            }
        } else {
            while (*ptr) {
                ptr = (uint64_t *)*ptr;
            }
        }
        CHECK_FLAGS;
        thread[tix].work_done += 1;
        sched_yield(); 
    }
}


void dirty_load(int tix) {
    uint64_t start = start_clock();
    sleep(1);
    uint64_t stop  = stop_clock();
    uint64_t tps = stop - start;
    srandom((unsigned int)(start & 0xffffffff));
    uint64_t *ptr = thread[tix].mem;
    uint64_t addresses = ((megabytes * MEGABYTE) / sizeof(uint64_t)) - 1;
    for (;;) {
        start = start_clock();
	for (int ix = 0;  (ix < dirty_rate);  ix++) {
            uint64_t r = ((random() << 31) | random()) % addresses;
            *(ptr + r) = start;
	}
        stop  = stop_clock();
        int64_t diff = stop - start;
        if ((diff > 0) && (diff < tps)) {
            // use select to pad out interval to one second
            uint64_t sleep_us = (1000000 * (tps - diff)) / tps;
            struct timeval tv;
            tv.tv_sec  = sleep_us / 1000000;
            tv.tv_usec = sleep_us % 1000000;
            int rc = 0;
            do {
                rc = select(0, NULL, NULL, NULL, &tv);
                if ((rc == -1) && (errno != EINTR)) {
                        CHECK_ERR(errno, "select");
                }
            } while (rc != 0);
        } else if (diff > tps) {
            fprintf(stderr, "Dirty rate taking %5.3g seconds -- dirty load timing might be off\n", (double)diff / (double)tps);
        }
        CHECK_FLAGS;
        thread[tix].work_done += 1;
        sched_yield();   // might not be necessary if positive delay above
    }
}


void sleep_load(int tix) {
    for (;;) {
        sleep(1);
        CHECK_FLAGS;
        thread[tix].work_done += 1;
    }
}


void spin_load(int tix) {
    uint64_t start = 0;
    uint64_t stop = 0;
    uint64_t diff = 0;
    uint64_t tps = 0;
    if (min_pct < 100) {
	start = start_clock();
	sleep(1);
	stop  = stop_clock();
	diff = stop - start;
	tps = diff / 1000000;
    }
    for (;;) {
	if (min_pct < 100) {
	    start = start_clock();
	}
	for (int ix = 0;  (ix < SPIN_LIMIT);  ix++) {
	    for (int iy = 0;  (iy < SPIN_LIMIT);  iy++) { }
            CHECK_FLAGS;
	}
        thread[tix].work_done += 1;
	if (min_pct >= 100) {
	    sched_yield(); 
	} else {
	    stop  = stop_clock();
	    diff = stop - start;
	    int pct = max_pct - min_pct;
	    if (pct <= 0) {
		pct = min_pct;
	    } else {
		pct = min_pct + (stop % pct); // make pct psuedo random
	    }
	    diff *= (100 - pct);
	    uint64_t denom = (pct * tps);
	    uint64_t sleep = diff / denom;
	    struct timeval tv;
	    tv.tv_sec  = sleep / 1000000;
	    tv.tv_usec = sleep % 1000000;
	    int rc = 0;
	    do {
		rc = select(0, NULL, NULL, NULL, &tv);
		if ((rc == -1) && (errno != EINTR)) {
			CHECK_ERR(errno, "select");
		}
	    } while (rc != 0);
	}
    }
}


void *run(void *arg) {
    int tix = *(int *)arg;
    int cpu = sched_getcpu();
    thread[tix].cpu[cpu] += 1;
    thread[tix].pid = getpid();
    if (use_global_mem) {
	thread[tix].mem = mem;
    } else if (megabytes) {
	thread[tix].mem = (uint64_t *)get_mem(megabytes);
    }
    if (debug) {
        printf("Thread %3d:  PID %d:  TID %12.12lx:  CPU: %3.3d  TOS near %p  MEM %p\n",
            tix, thread[tix].pid, thread[tix].tid, cpu, &tix, thread[tix].mem);
    }
    if (do_fpu_load)   {   fpu_load(tix); }
    if (do_int_load)   {   int_load(tix); }
    if (do_mem_load)   {   mem_load(tix); }
    if (do_dirty_load) { dirty_load(tix); }
    if (do_sleep_load) { sleep_load(tix); }
    if (do_spin_load)  {  spin_load(tix); }
    if (debug) {
        printf("Thread %3d:  PID %d:  TID %12.12lx:  CPU: %3.3d  TOS near %p  MEM %p  Load complete\n",
            tix, thread[tix].pid, thread[tix].tid, cpu, &tix, thread[tix].mem);
    }
    return NULL;
}


void make_threads(int proc_ix) {
    if (round_robin) {
        int node_ix = proc_ix % num_nodes;
        if (sched_setaffinity(0, node[node_ix].cpu_set_size, node[node_ix].cpu_set_p) < 0) {
            perror("sched_setaffinity");
            exit(EXIT_FAILURE);
        }
        sched_yield();
    }
    if ((megabytes) && (use_global_mem)) {
        // allocate process-wide memory
	mem = (uint64_t *)get_mem(megabytes);
    }
    pthread_attr_t attr;
    int rc = pthread_attr_init(&attr);
    CHECK_ERR(rc, "pthread_attr_init");
    thread = malloc(threads * sizeof(thread_data_t));
    if (thread == NULL) {
        CHECK_ERR(ENOMEM, "malloc");
    }
    memset(thread, 0, threads * sizeof(thread_data_t));
    // Start at 1, because of existing "thread"
    for (int ix = 1;  (ix < threads);  ix++) {
        thread[ix].tix = ix;
        rc = pthread_create(&thread[ix].tid, &attr, &run, &thread[ix].tix);
        CHECK_ERR(rc, "pthread_create");
    }
    signal(SIGINT, sigint_handler);
    signal(SIGALRM, sigalrm_handler);
    alarm(ALARM_INTERVAL);
    // thread[0].tix was initialized to 0, so no need to set
    rc = 0;
    run(&rc);  // existing "thread"
    // rejoin all threads
    for (int ix = 1;  (ix < threads);  ix++) {
        rc = pthread_join(thread[ix].tid, NULL);
        CHECK_ERR(rc, "pthread_join");
        if (debug) {
            printf("Thread %d joined\n", thread[ix].tix);
        }
    }
    dump_thread_data();
    free(thread);
    rc = pthread_attr_destroy(&attr);
    CHECK_ERR(rc, "pthread_attr_destroy");
}


void make_processes() {
    // Start at 1, because of existing process
    for (int ix = 1;  (ix < processes);  ix++) {
        pid_t pid = fork();
        if (pid < 0) {
            CHECK_ERR(errno, "fork");
        }
        if (pid == 0) {
            make_threads(ix);
            return;
        }
    }
    make_threads(0);  // with existing process
}


int main(int argc, char *argv[]) {
    int opt;
    while ((opt = getopt(argc, argv, "dGhHi:Ik:l:L:m:MNn:p:qrs:St:TvVw")) != -1) {
	switch (opt) {
	    case 'd': debug      = 1; break;
	    case 'G': use_global_mem = 1; break;
	    case 'h': print_usage_and_exit(argv[0]); break;
            case 'H': use_huge_pages  = 1; break;
	    case 'i': {
                char *p = NULL;
		work_seconds = (int)strtol(optarg, &p, 10);
                if (p == optarg) {
		    printf("Can't parse work_seconds: %s\n", optarg);
		    exit(EXIT_FAILURE);
		}
                rest_seconds = work_seconds;
		save_work_seconds = work_seconds;
		if (*p == ':') {
		    char *rest_seconds_str = p + 1;
		    rest_seconds = (int)strtol(rest_seconds_str, &p, 10);
		    if (p == rest_seconds_str) {
			printf("Can't parse rest_seconds: %s\n", rest_seconds_str);
			exit(EXIT_FAILURE);
		    }
		}
                break;
            }
	    case 'I': use_interleaved_mem = 1; break;
	    case 'k': {
                char *p = NULL;
		min_pct = (int)strtol(optarg, &p, 10);
                if (p == optarg) {
		    printf("Can't parse min_pct: %s\n", optarg);
		    exit(EXIT_FAILURE);
		}
                max_pct = min_pct;
		if (*p == ':') {
		    char *max_str = p + 1;
		    max_pct = (int)strtol(max_str, &p, 10);
		    if (p == max_str) {
			printf("Can't parse max_pct: %s\n", max_str);
			exit(EXIT_FAILURE);
		    }
		}
                if (min_pct < 1) { min_pct = 1; }
                if (min_pct > 100) { min_pct = 100; }
		if (max_pct < min_pct) { max_pct = min_pct; };
                if (max_pct > 100) { max_pct = 100; }
                break;
            }
	    case 'l':
                do_spin_load  = !strcmp(optarg, "spin");
                do_sleep_load = !strcmp(optarg, "sleep");
                do_mem_load   = !strcmp(optarg, "mem");
                do_dirty_load = !strcmp(optarg, "dirty");
                do_int_load   = !strcmp(optarg, "int");
                do_fpu_load   = !strcmp(optarg, "fpu");
            break;
	    case 'L': mem_stride  = atoi(optarg); break;
	    case 'm': megabytes   = atoi(optarg); break;
	    case 'M': use_mergeable_mem = 1; break;
	    case 'n': dirty_rate  = atoi(optarg); break;
	    case 'N': avoid_THP_mem     = 1; break;
	    case 'p': processes   = atoi(optarg); break;
	    case 'q': quiet       = 1; break;
	    case 'r': round_robin = 1; break;
	    case 's': seconds     = atoi(optarg); break;
	    case 'S': use_shared_mem = 1; break;
	    case 't': threads     = atoi(optarg); break;
	    case 'T': use_THP_mem = 1; break;
	    case 'v': verbose     = 1; break;
	    case 'V': print_version_and_exit(argv[0]); break;
	    case 'w': also_write_mem = 1; break;
	    default: print_usage_and_exit(argv[0]); break;
	}
    }
    if (argc > optind) {
	printf("Unexpected arg = %s\n", argv[optind]);
	exit(EXIT_FAILURE);
    }
    if (use_shared_mem && (use_mergeable_mem || use_THP_mem)) {
	printf("Shared memory cannot also be huge or mergeable.\n");
	exit(EXIT_FAILURE);
    }
    num_cpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (num_cpus > MAX_CPUS) {
        printf("static CPU table too small\n");
	exit(EXIT_FAILURE);
    }
    if ((do_mem_load || do_dirty_load) && (megabytes == 0)) {
        printf("Must specify megabytes for MEM or DIRTY load.\n");
	exit(EXIT_FAILURE);
    }
    if (do_mem_load) {
        if (mem_stride < 1) {
            printf("Length of memory stride must be > 1.\n");
            exit(EXIT_FAILURE);
        }
        if (also_write_mem && (mem_stride < 2)) {
            printf("If also writing memory, length of memory stride must be > 2.\n");
            exit(EXIT_FAILURE);
        }
        if (mem_stride > (((megabytes * MEGABYTE) / sizeof(uint64_t *)) - 2)) {
            printf("Length of memory stride is bigger than memory allocated.\n");
            exit(EXIT_FAILURE);
        }
    }
    num_nodes = get_nodes();
    if (verbose) {
        printf("system has %d nodes\n", num_nodes);
        printf("system has %d cpus\n", num_cpus);
        printf("processes  = %d\n", processes);
        printf("threads    = %d\n", threads);
        if (megabytes > 0) {
            printf("megabytes  = %d\n", megabytes);
        }
        if (do_mem_load) {
            printf("mem stride = %d\n", mem_stride);
        }
        if (seconds > 0) {
            printf("seconds    = %d\n", seconds);
        }
        if (work_seconds > 0) {
            printf("work_secs  = %d\n", work_seconds);
            printf("rest_secs  = %d\n", rest_seconds);
        }
        if (do_spin_load) {
            printf("min_pct    = %d\n", min_pct);
            printf("max_pct    = %d\n", max_pct);
        }
        fflush(stdout);
    }
    sem_mutex_ptr = sem_open("Proc_Dump_Semaphore", O_CREAT | O_EXCL, 0644, 1);
    if (sem_mutex_ptr == SEM_FAILED) {
        perror("sem_open");
        exit(EXIT_FAILURE);
    }
    if (sem_unlink("Proc_Dump_Semaphore") < 0) {
        perror("sem_unlink");
        exit(EXIT_FAILURE);
    }
    make_processes();
    if (sem_destroy(sem_mutex_ptr) < 0) {
        perror("sem_destroy");
    }
    exit(EXIT_SUCCESS);
}

