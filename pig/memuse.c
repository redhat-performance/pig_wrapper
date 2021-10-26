// cc -g -std=gnu99 -o memuse memuse.c

#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <math.h>
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

#define MEGABYTE (uint64_t)1024 * 1024
#define DEFAULT_MEM_SIZE 511 * MEGABYTE
#define STRIDE 57

size_t megabytes = 0;
int seconds = 0;
int quiet = 0;


void sigalrm_handler(int sig) {
    exit(EXIT_SUCCESS);
}


void print_usage_and_exit(char *progname) {
    fprintf(stderr, "Usage: %s blah blah blah\n", progname);
    exit(EXIT_FAILURE);
}


char *get_mem(size_t mem_size) {
    char *mem = mmap(0, mem_size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, 0, 0);
    if (mem == MAP_FAILED) {
	perror("mmap");
	exit(EXIT_FAILURE);
    }
    return mem;
}


int main(int argc, char *argv[]) {
    int opt;
    while ((opt = getopt(argc, argv, "m:qs:")) != -1) {
	switch (opt) {
	    case 'm': megabytes = atoi(optarg); break;
	    case 's': seconds   = atoi(optarg); break;
	    case 'q': quiet     = 1; break;
	    default: print_usage_and_exit(argv[0]); break;
	}
    }
    if (argc > optind) {
	printf("Unexpected arg = %s\n", argv[optind]);
	exit(EXIT_FAILURE);
    }
    size_t mem_size = DEFAULT_MEM_SIZE;
    if (megabytes) {
        mem_size = megabytes * MEGABYTE;
    }
    uint64_t *mem = (uint64_t *)get_mem(mem_size);
    // make pointer chain through memory
    uint64_t *ptr = mem;
    int num_words = mem_size / sizeof(uint64_t *);
    while (ptr < mem + num_words - (STRIDE + 1)) {
	*ptr = (uint64_t)(ptr + STRIDE);
	ptr = (uint64_t *)*ptr;
    }
    *ptr = (uint64_t)NULL;
    
    // Set alarm if seconds specified
    if (seconds) {
	signal(SIGALRM, sigalrm_handler);
	alarm(seconds);
    }
    // Loop forever -- or until alarm -- either sleeping or reading memory
    for (;;) {
        if (quiet) {
            sleep(3);
        } else { 
            ptr = mem;
            while (*ptr) {
                ptr = (uint64_t *)*ptr;
            }
        }
    }

/*
    // free memory
    if (munmap(mem, mem_size) < 0) {
	perror("munmap");
	exit(EXIT_FAILURE);
    }
*/
    exit(EXIT_SUCCESS);
}

