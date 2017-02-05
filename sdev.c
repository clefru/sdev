/*
 *
 * sdev.c - Clemens Fruhwirth <clemens@endorphin.org>
 *
 * This file may be redistributed under the terms of the GPL
 *
 * 0.99 initial release
 *
 */

#define _GNU_SOURCE
#define _LARGEFILE_SOURCE
#define _LARGEFILE64_SOURCE
#define FILE_OFFSET_BITS 64

#include <assert.h>
#include <stdlib.h>
#include <stropts.h>
#include <fcntl.h>
#include <features.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
//#include <asm/fcntl.h>
#include <linux/fs.h>		// to determinate blksize we need to
				// ioctl call the device with appropriate
				// cmd id - that's why we're OS dependend.
#include <assert.h>
#include <string.h>
#include <getopt.h>
#ifdef GSL_RNG
#include <gsl/gsl_rng.h>
#endif
#include <netinet/in.h>
#include <signal.h>
#include <time.h>

#define SEED_SIZE 32
#define SIGNATURE "SDEVBLOCK"
#define SIGNATURE_SIZE 10

// mt19937 functions
void init_by_array(unsigned long init_key[],int key_length);
unsigned long genrand_int32(void);


// I tried a OO coding style in C, but I somehow lost interest half-way
// don't be surprised about the strange constructs

struct block {
  char          *name;
  unsigned int  fd;
  int           blocksize;
  uint64_t      size;
  char          initialized;
};

struct resumeInfo_t {
  char      signature[SIGNATURE_SIZE];
  int       loop;
  off64_t   offset;
};

int block_getBlockSize(struct block *this) {
  assert(this->initialized);
  return(this->blocksize);
}

int block_write(struct block *this, char *data, int size) {
  int rc;
  assert(this->initialized);

  if(size % this->blocksize != 0) {
    printf("block_write: size is not a multiply of blocksize.\n");
    return -1;
  }

  rc = write(this->fd, data, size);
  return rc;
}

void block_setOffset(struct block *this, off64_t offset, int whence) {
  assert(this->initialized);
  (void)lseek64(this->fd, offset, whence);
}

off64_t block_getOffset(struct block *this) {
  assert(this->initialized);
  return lseek64(this->fd, 0, SEEK_CUR);
}

struct block * block_block(const char *name,int mode) {
  int fd;
  struct block *this;
  struct stat stat;
  
  if((fd = open(name,mode))<0) { 
    perror("block_block");
    return NULL;
  }
  if((this = (struct block *)malloc(sizeof(struct block)))==NULL) { 
    (void)printf("block_block: can't malloc sizeof(struct block)=%lu\n",
                 sizeof(struct block));
    return NULL;
  }
  this->name = strdup(name);
  this->fd = fd;
  if(ioctl(fd, BLKGETSIZE64, &(this->size))<0) {
    perror("ioctl");
    return(NULL);
  }
  if(fstat(fd, &stat)<0) {
    perror("fstat");
    return(NULL);
  }
  this->blocksize = stat.st_blksize;
  this->initialized = 1;
  return(this);
}

void block_0block(struct block *this) {
  assert(this->initialized);
	
  this->initialized = 0;
  free(this->name);
  (void)close(this->fd);
}

void special(char *buffer, int buffer_size, int turn) {
  int i;
  unsigned char write_modes[27][3] = {
    {"\x55\x55\x55"}, {"\xaa\xaa\xaa"}, {"\x92\x49\x24"}, 
    {"\x49\x24\x92"}, {"\x24\x92\x49"}, {"\x00\x00\x00"}, 
    {"\x11\x11\x11"}, {"\x22\x22\x22"}, {"\x33\x33\x33"}, 
    {"\x44\x44\x44"}, {"\x55\x55\x55"}, {"\x66\x66\x66"}, 
    {"\x77\x77\x77"}, {"\x88\x88\x88"}, {"\x99\x99\x99"}, 
    {"\xaa\xaa\xaa"}, {"\xbb\xbb\xbb"}, {"\xcc\xcc\xcc"}, 
    {"\xdd\xdd\xdd"}, {"\xee\xee\xee"}, {"\xff\xff\xff"}, 
    {"\x92\x49\x24"}, {"\x49\x24\x92"}, {"\x24\x92\x49"}, 
    {"\x6d\xb6\xdb"}, {"\xb6\xdb\x6d"}, {"\xdb\x6d\xb6"}
  };
  
  assert((buffer_size % 3) == 0);
  for(i = 0; i < buffer_size/3; i++) {
    memcpy(buffer,write_modes[turn], 3);
    buffer += 3;
  }
}

void write_resumeinfo(struct block *block, int i, struct resumeInfo_t *resInfo) {
  off64_t res;
  int rc;
  res = block_getOffset(block);
  resInfo->offset = res;
  resInfo->loop = i;
  
  block_setOffset(block, 0, SEEK_SET);
  if(write(block->fd, resInfo, block_getBlockSize(block)) != block_getBlockSize(block)) {
    printf("Warning failed to write resumeinfo!\n");
  }
  block_setOffset(block, res, SEEK_SET);
}

void verbose(char *buffer) {
  static int lastoutput = 0;
  int i;

  // Go back one character, clear it by printing a space, then go back again
  for(i = 0;i<lastoutput;i++)
    printf("\b \b");
  
  lastoutput = strlen(buffer);
  printf("%s", buffer);
  fflush(NULL);
}

ssize_t mt19937(int rfd, void *__block, size_t blocksize) {
  unsigned long *block = (unsigned long *)__block;
  int i;
  assert((blocksize % sizeof(unsigned long))==0);
  for(i = 0;i<blocksize/sizeof(unsigned long);i++)
    *(block+i) = genrand_int32();
  return blocksize;
}

/* Credits go to Michal's padlock patches for this alignment code */
static void *aligned_malloc(char **base, int size, int alignment) {
  char *ptr;
  
  ptr = malloc(size + alignment);
  if(ptr == NULL) return NULL;
  
  *base = ptr;
  if(alignment > 1 && ((long)ptr & (alignment - 1))) {
    ptr += alignment - ((long)(ptr) & (alignment - 1));
  }
  return ptr;
}

#define RETRIGGER_TIME 5

int *global_i;
struct block *global_mine;
struct resumeInfo_t *global_resumeInfo;

void status_update(int foo) {
  char *verboseBuffer;
  static uint64_t lastOffset = 0;
  static time_t lastTime = 0;
  time_t newTime = time(NULL);
  fsync(global_mine->fd);
  write_resumeinfo(global_mine, *global_i, global_resumeInfo);
  
  assert(asprintf(&verboseBuffer,
		  "Run: %d/39, offset %ld, size %ld, percent: %2.0f, speed %ld",
		  *global_i, global_resumeInfo->offset, global_mine->size,
		  ((double)global_resumeInfo->offset*100.0/global_mine->size),
		  (global_resumeInfo->offset-lastOffset)/(newTime - lastTime)) != -1);
  verbose(verboseBuffer);
  free(verboseBuffer);
  lastOffset = global_resumeInfo->offset;
  lastTime = newTime;
  alarm(RETRIGGER_TIME);
}

int main(int argc, char **argv) {
  struct block *mine;
  char *buffer; char *buffer_base; int buffer_size; 
  int i, force = 0;
  char *targetbdev = NULL;
  struct resumeInfo_t *resumeInfo;
  char *base_resumeInfo;
  int urandom;
  ssize_t (*random)(int fd, void *block, size_t blocksize) = read;
  char seedbuffer[SEED_SIZE];
	
  urandom = open("/dev/urandom", O_RDONLY);
  if(urandom == -1) {
    perror("open");
    return -1;
  }		
  while(1) {
    int this_option_optind = optind ? optind : 1;
    int option_index = 0;
    int c;
    static struct option long_options[] = {
      {"force", 0, 0, 'f'},
      {"mersenne", 0, 0, 'm'},
      {0, 0, 0, 0}
    };
    c = getopt_long (argc,  argv, "rm",
                     long_options, &option_index);
    if (c == -1)
      break;
    
    switch (c) {
    case 'f':
      force = 1;
      break;
    case 'm':
      urandom = open("/dev/random", O_RDONLY);
      assert(read(urandom, seedbuffer, SEED_SIZE) == SEED_SIZE);
      init_by_array((unsigned long *)seedbuffer, SEED_SIZE/sizeof(unsigned long));
      close(urandom);
      random = mt19937;
      break;
    default:
      printf ("getopt returned character code 0%o ??\n", c);
    }
  }
  
  if(argc - optind != 1) { 
    printf("usage: %s <blockdevice>\n", argv[0]);
    return 1;
  } else
    targetbdev = argv[optind++];
  
  
  if((mine = block_block(targetbdev, O_RDWR | __O_DIRECT))==NULL) {
    printf("construction of block object failed\n.");
    return 1;
  }
#define BUF_MULTIPLIER 100
  buffer_size = block_getBlockSize(mine)*3*BUF_MULTIPLIER;
  if((buffer = (char *)aligned_malloc(&buffer_base, buffer_size, block_getBlockSize(mine)))==NULL) {
    //	if((buffer=(char *)malloc(buffer_size))==NULL) {
    (void)perror("malloc");
    return -1;
  }
  
  /* Check resume info */
  assert(sizeof(struct resumeInfo_t) < block_getBlockSize(mine));
  resumeInfo = aligned_malloc(&base_resumeInfo, block_getBlockSize(mine), block_getBlockSize(mine));
  block_setOffset(mine, 0, SEEK_SET);
  assert(read(mine->fd, resumeInfo, block_getBlockSize(mine)) == block_getBlockSize(mine));
  if(strcmp(resumeInfo->signature, SIGNATURE) || force) {
    printf("Starting purge operations on %s\n", targetbdev);
    block_setOffset(mine, block_getBlockSize(mine), SEEK_SET);
    strcpy(resumeInfo->signature, SIGNATURE);
    i = 0;
  } else {
    printf("Resumed operation: loop: %d offset: %ld\n", resumeInfo->loop, resumeInfo->offset);
    i = resumeInfo->loop;
    block_setOffset(mine, resumeInfo->offset, SEEK_SET);
  }
  
  /* Set up alarm trigger */
  global_i = &i;
  global_mine = mine;
  global_resumeInfo = resumeInfo;
  
  signal(SIGALRM, status_update);
  alarm(RETRIGGER_TIME);
  
  for(;i<39;i++) {
    do {
      if(i>=0  && i<5) random(urandom, buffer, buffer_size);
      else if(i>=5  && i<33) special(buffer, buffer_size, i);
      else if(i>=33 && i<38) random(urandom, buffer, buffer_size);
      else if(i>=38 && i<39) memset(buffer, 0xFF, buffer_size);
    } while(block_write(mine, buffer, buffer_size)==buffer_size);
    
    block_setOffset(mine, block_getBlockSize(mine), SEEK_SET);
  }
  // Kill first sector
  for(i=0;i<39;i++) {	
    if(i>=0  && i<5) random(urandom, buffer, buffer_size);
    if(i>=5  && i<33) special(buffer, buffer_size, i);
    if(i>=33 && i<38) random(urandom, buffer, buffer_size);
    if(i>=38 && i<39) memset(buffer, 0xFF, buffer_size);
    assert(block_write(mine, buffer, buffer_size) == buffer_size);
    fsync(mine->fd);
    block_setOffset(mine, 0, SEEK_SET);
  }
  block_0block(mine);
  free(buffer_base);
  free(base_resumeInfo);
  close(urandom);
}
