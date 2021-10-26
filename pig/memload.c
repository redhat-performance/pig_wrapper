//
// compile with: gcc -std=gnu99 -g -Wall -pthread -o memload memload.c 
//

#define _GNU_SOURCE
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <values.h>

#define CHECK_ERR(rc, msg) \
    if (rc != 0) { \
        errno = rc; \
	perror(msg); \
	exit(EXIT_FAILURE); \
    }

int processes = 1;
int threads = 1;
int megabytes = 0;
int seconds = 0;

typedef struct thread_data {
    uint64_t *mem;
    pthread_t tid;
    int tix;
} thread_data_t, *thread_data_p;
thread_data_p thread = NULL;

void sigalrm_handler(int sig) {
    exit(EXIT_SUCCESS);
}

char *get_mem(int megabytes) {
    int flags = MAP_ANONYMOUS | MAP_PRIVATE;
    size_t mem_size = megabytes * 1024 * 1024;
    char *mem = mmap(NULL, mem_size, PROT_READ | PROT_WRITE, flags, 0, 0);
    if (mem == MAP_FAILED) {
	perror("mmap");
	exit(EXIT_FAILURE);
    }
    // make pointer chain through the memory
#define STRIDE 57
    uint64_t *ptr = (uint64_t *)mem;
    uint64_t num_words = mem_size / sizeof(uint64_t *);
    while (ptr < (uint64_t *)mem + num_words - (STRIDE + 1)) {
	*ptr = (uint64_t)(ptr + STRIDE);
	ptr = (uint64_t *)*ptr;
    }
    *ptr = (uint64_t)NULL;
    return mem;
}

void *run(void *arg) {
    int tix = *(int *)arg;
    thread[tix].mem = (uint64_t *)get_mem(megabytes);
    for (;;) {
        uint64_t *ptr = thread[tix].mem;
        while (*ptr) {
            ptr = (uint64_t *)*ptr;
        }
        sched_yield(); 
    }
    return NULL;
}

void make_threads(int proc_ix) {
    printf("%d\n", getpid());
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
    signal(SIGALRM, sigalrm_handler);
    alarm(seconds);
    fflush(stdout);
    rc = 0;
    run(&rc);  // existing "thread"
    // rejoin all threads
    for (int ix = 1;  (ix < threads);  ix++) {
        rc = pthread_join(thread[ix].tid, NULL);
        CHECK_ERR(rc, "pthread_join");
    }
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
    processes = 5;
    threads = 6;
    megabytes = 1000;
    seconds = 500;
    make_processes();
    exit(EXIT_SUCCESS);
}

