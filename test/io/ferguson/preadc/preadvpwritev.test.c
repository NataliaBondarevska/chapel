#include "sys_basic.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/uio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>

int main() {
  struct iovec vec[1];
  char* buf;
  size_t buflen = 4096;
  char* data = "aaaaaaaaaaaaaaaaaaaa";
  int datalen = strlen(data);
  int fd;
  ssize_t got;

  
  buf = malloc(buflen);
  assert(buf);
  vec[0].iov_base = buf;
  vec[0].iov_len = buflen;

  // open file
  fd = open("tmp.data", O_RDWR | O_CREAT, S_IRWXU);
  assert(fd>=0);
  
  // copy data to buf
  memset(buf, 0, buflen);
  memcpy(buf, data, datalen);
  vec[0].iov_base = buf;
  vec[0].iov_len = datalen;
  // write data
  got = pwritev(fd, vec, 1, 0);
  //got = pwrite(fd, buf, datalen, 0);
  if( got == - 1 ) perror("in write");
  assert(got == datalen);
 
  // clear buf
  memset(buf, 0, buflen);
  // read data
  vec[0].iov_base = buf;
  vec[0].iov_len = buflen;
  got = preadv(fd, vec, 1, 0);
  //got = pread(fd, buf, buflen, 0);
  if( got == - 1 ) perror("in read");
  assert(got == datalen);
  // check data
  assert(0 == memcmp(data, buf, datalen));
 
  close(fd);
  unlink("tmp.data");

  printf("OK\n");

  return 0;
}

