#include <stdio.h>
#include <sys/mman.h>
#include <stdlib.h>

#define MB_1 (1024*1024)
#define GB_1 (1024*MB_1)

void wr_to_array(size_t size, char *p)
{
  size_t i;
  for( i=0 ; i<size ; i++) {
    p[i] = 'A';
  }
}

void rd_from_array(size_t size, char *p)
{
  size_t i, count = 0;
  for( i=0 ; i<size ; i++)
    if (p[i] == 'A') count++;
  if (count==i)
    printf("Anon read success :-)\n");
  else
    printf("Anon read failed :-(\n");
}

void usage(char *prog)
{
  printf("%s <Memory in GB to be allocated>\n\nExample: %s 2\n\tAllocates 2GB of memory\n",prog,prog);
  exit(-1);
}


int main(int argc, char *argv[])
{
  int count,i;
  size_t size;
  char *p[1024];

  if (argc !=2)
	usage(argv[0]);
  count = atoi(argv[1]);
  if (count<1 || count>1024)
	usage(argv[0]);
  size = GB_1;

  for (i=0;i<count;i++)
  {
    p[i] = mmap (NULL, size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (p[i] == MAP_FAILED) {
      perror("MMAP failed");
      exit(-2);
    }
  }
  printf("Anonymous memory segment initialized !\n");
  printf("Press any key to write to memory area\n");
  getchar();
  for (i=0;i<count;i++)
    wr_to_array(size,p[i]);
  printf("Press any key to rd from memory area\n");
  getchar();
  for (i=0;i<count;i++)
    rd_from_array(size,p[i]);
  printf("Press any key to finish\n");
  getchar();
  for (i=0;i<count;i++)
    munmap(p[i], size);
  return 0;
}
