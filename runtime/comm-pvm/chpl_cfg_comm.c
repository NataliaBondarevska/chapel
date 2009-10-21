#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>

#include "pvm3.h"

#include "chplcgfns.h"
#include "chplrt.h"
#include "chplcomm.h"
#include "chpl_mem.h"
#include "chplsys.h"
#include "chplthreads.h"
#include "chpltypes.h"
#include "error.h"

#define CHPL_DIST_DEBUG 0

#if CHPL_DIST_DEBUG
#define DEBUG_MSG_LENGTH 256
#define PRINTF(_s) {                                                       \
  printf("%d/%d:%d/%d:%s\n", chpl_localeID, chpl_numLocales,               \
                             (int)getpid(), (int)pthread_self(), _s);      \
  fflush(stdout);                                                          \
}
#else
#define PRINTF(_s)
#endif

int lock_num = 0;
#define PVM_SAFE(call, who, in) {                                          \
  int retcode;                                                             \
  CHPL_MUTEX_LOCK(&pvm_lock);                                              \
  retcode = call;                                                          \
  CHPL_MUTEX_UNLOCK(&pvm_lock);                                            \
  lock_num++;                                                              \
  if (retcode < 0) {                                                       \
    char msg[256];                                                         \
    sprintf(msg, "\n\n%d/%d:%d PVM call %s failed with %d in %s.\n\n", chpl_localeID, chpl_numLocales, (int)pthread_self(), who, retcode, in);             \
    chpl_error(msg, 0, 0);                                                 \
  }                                                                        \
}

#define PVM_PACK_SAFE(call, who, in) {                                     \
  int retcode;                                                             \
  CHPL_MUTEX_LOCK(&pvm_lock);                                              \
  retcode = call;                                                          \
  lock_num++;                                                              \
  if (retcode < 0) {                                                       \
    char msg[256];                                                         \
    sprintf(msg, "\n\n%d/%d:%d PVM call %s failed with %d in %s.\n\n", chpl_localeID, chpl_numLocales, (int)pthread_self(), who, retcode, in);             \
    chpl_error(msg, 0, 0);                                                 \
  }                                                                        \
}

#define PVM_UNPACK_SAFE(call, who, in) {                                   \
  int retcode;                                                             \
  retcode = call;                                                          \
  CHPL_MUTEX_UNLOCK(&pvm_lock);                                            \
  if (retcode < 0) {                                                       \
    char msg[256];                                                         \
    sprintf(msg, "\n\n%d/%d:%d PVM call %s failed with %d in %s.\n\n", chpl_localeID, chpl_numLocales, (int)pthread_self(), who, retcode, in);             \
    chpl_error(msg, 0, 0);                                                 \
  }                                                                        \
}

#define PVM_NO_LOCK_SAFE(call, who, in) {                                  \
  int retcode;                                                             \
  retcode = call;                                                          \
  if (retcode < 0) {                                                       \
    char msg[256];                                                         \
    sprintf(msg, "\n\n%d/%d:%d PVM call %s failed with %d in %s.\n\n", chpl_localeID, chpl_numLocales, (int)pthread_self(), who, retcode, in);             \
    chpl_error(msg, 0, 0);                                                 \
  }                                                                        \
}

#define PVM_SAFE_OKAY_TO_FAIL(call, who, in) {                             \
  int retcode;                                                             \
  CHPL_MUTEX_LOCK(&pvm_lock);                                              \
  retcode = call;                                                          \
  CHPL_MUTEX_UNLOCK(&pvm_lock);                                            \
  lock_num++;                                                              \
}

#define TAGMASK 4194303
#define BCASTTAG 4194299
#define NOTIFYTAG 4194295

chpl_mutex_t pvm_lock;
chpl_mutex_t termination_lock;

static chpl_mutex_t chpl_comm_diagnostics_lock;
static int chpl_comm_gets = 0;
static int chpl_comm_puts = 0;
static int chpl_comm_forks = 0;
static int chpl_comm_nb_forks = 0;
static int chpl_comm_no_debug_private = 0;

int parent = -23;   // used to send messages back to launcher
int tids[64]; // tid list for all nodes
int instance;

int okay_to_barrier = 1;
int okaypoll = 0;
pthread_mutex_t okay_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t okay_to_poll = PTHREAD_COND_INITIALIZER;
int commsig = 0;     // signal to parent process what to do
                     // 0: noop
                     // 1: halt
                     // 2: fprintf
                     // 3: printf
                     // 4: gdb

extern int fileno(FILE *stream);
extern const int chpl_max_fields_per_type;

//
// Chapel interface starts here
//


int32_t chpl_comm_getMaxThreads(void) {
  return 0;
}

int32_t chpl_comm_maxThreadsLimit(void) {
  return 0;
}

typedef enum {
  ChplCommPut,
  ChplCommGet,
  ChplCommFork,
  ChplCommForkNB,
  ChplCommFinish,
  ChplCommBroadcastPrivate
} ChplCommMsgType;

//
// Sent over network for every chpl_comm_get, _put, _fork, or _fork_nb.
// On normal Linux64 configurations, this is 16 bytes.
//
typedef struct __chpl_message_info {
  int msg_type : 4;     // One of the ChplCommMsgType enum values
  int replyTag : 27;    // Use this tag for the reply message
  int size;             // Size of data or function arguments
  union {
    void* data;         // Data location
    chpl_fn_int_t fid;  // Function ID
  } u;
} _chpl_message_info;

//
// A function and argument for a remote procedure call. When done executing
// the function, send and empty message to joinLocale with replyTag.
//
typedef struct __chpl_RPC_arg {
  chpl_fn_int_t fid;
  void* arg;
  int replyTag;
  int joinLocale;
} _chpl_RPC_arg;

static void chpl_RPC(_chpl_RPC_arg* arg);
static int chpl_pvm_recv(int tid, int msgtag, void* buf, int sz);
static void chpl_pvm_send(int tid, int msgtag, void* buf, int sz);
static int makeTag(int threadID, int localeID);

int unique_tag = 1;
// Just need something unique per process
static int makeTag(int threadID, int localeID) {
  int tag;
  int absTid = threadID < 0 ? -threadID : threadID;
  tag = unique_tag + (1000 * localeID) + (100 * (absTid>>21));
  unique_tag++;
  while (tag > 4194294) (tag = tag - (100 * localeID) - (10 * (absTid>>21)));
  if (unique_tag > 4194294) unique_tag = 1;
  return tag;
}

static void chpl_RPC(_chpl_RPC_arg* arg) {
  PRINTF("Entering forked task");
  (*chpl_ftable[arg->fid])(arg->arg);
  PRINTF("Did task");
  chpl_pvm_send(arg->joinLocale, arg->replyTag, NULL, 0);
  if (arg->arg != NULL) {
    chpl_free(arg->arg, 0, 0);
  }
  chpl_free(arg, 0, 0);
}

#ifdef CHPL_COMM_HETEROGENEOUS
/////////////////////////////////////////////////////////////////////////////
// int16_t
// No matter what (32->32, 32->64, or 64->32), the sender is just sending
// 16 bits, and the receiver receives 16 bits. No conversions should be
// necessary.
static void chpl_pkint16_t(void* buf, int i, int chpltypetype, unsigned long chpltypeoffset) {
#if CHPL_DIST_DEBUG
  char debugMsg[DEBUG_MSG_LENGTH];
#endif

  PVM_NO_LOCK_SAFE(pvm_pkshort((int16_t *)(((char *)buf)+chpltypeoffset), 1, 1), "pvm_pkshort", "chpl_pvm_send");
#if CHPL_DIST_DEBUG
  if ((((char *)buf)+chpltypeoffset) != 0) {
    sprintf(debugMsg, "Packing int16_t %hd (part %d) of type %d, offset %lu", *(short int *)(((char *)buf)+chpltypeoffset), i, chpltypetype, chpltypeoffset);
    PRINTF(debugMsg);
  }
#endif

  return;
}

static void chpl_upkint16_t(void* buf, int i, int chpltypetype, unsigned long chpltypeoffset) {
#if CHPL_DIST_DEBUG
  char debugMsg[DEBUG_MSG_LENGTH];
#endif

  PVM_NO_LOCK_SAFE(pvm_upkshort((int16_t *)(((char *)buf)+chpltypeoffset), 1, 1), "pvm_upkshort", "chpl_pvm_recv");
#if CHPL_DIST_DEBUG
  sprintf(debugMsg, "Unpacking int16_t %hd (part %d) of type %d, offset %lu", *(short int *)(((char *)buf)+chpltypeoffset), i, chpltypetype, chpltypeoffset);
  PRINTF(debugMsg);
#endif

  return;
}
/////////////////////////////////////////////////////////////////////////////

/////////////////////////////////////////////////////////////////////////////
// int32_t
// No matter what (32->32, 32->64, or 64->32), the sender is just sending
// 32 bits, and the receiver receives 32 bits. No conversions should be
// necessary.
static void chpl_pkint32_t(void* buf, int i, int chpltypetype, unsigned long chpltypeoffset) {
#if CHPL_DIST_DEBUG
  char debugMsg[DEBUG_MSG_LENGTH];
#endif

  PVM_NO_LOCK_SAFE(pvm_pkint((int32_t *)(((char *)buf)+chpltypeoffset), 1, 1), "pvm_pkint", "chpl_pvm_send");
#if CHPL_DIST_DEBUG
  if ((((char *)buf)+chpltypeoffset) != 0) {
    sprintf(debugMsg, "Packing int32_t %d (part %d) of type %d, offset %lu", *(int *)(((char *)buf)+chpltypeoffset), i, chpltypetype, chpltypeoffset);
    PRINTF(debugMsg);
  }
#endif

  return;
}

static void chpl_upkint32_t(void* buf, int i, int chpltypetype, unsigned long chpltypeoffset) {
#if CHPL_DIST_DEBUG
  char debugMsg[DEBUG_MSG_LENGTH];
#endif

  PVM_NO_LOCK_SAFE(pvm_upkint((int32_t *)(((char *)buf)+chpltypeoffset), 1, 1), "pvm_upkint", "chpl_pvm_recv");
#if CHPL_DIST_DEBUG
  sprintf(debugMsg, "Unpacking int32_t %d (part %d) of type %d, offset %lu", *(int *)(((char *)buf)+chpltypeoffset), i, chpltypetype, chpltypeoffset);
  PRINTF(debugMsg);
#endif

  return;
}
/////////////////////////////////////////////////////////////////////////////

/////////////////////////////////////////////////////////////////////////////
// int64_t
// If the receiver is 32-bit, conversion is necessary.
static void chpl_pkint64_t(void* buf, int i, int chpltypetype, unsigned long chpltypeoffset) {
  int part1, part2;
#if CHPL_DIST_DEBUG
  char debugMsg[DEBUG_MSG_LENGTH];
#endif

  part1 = (int)*(int64_t *)(((char *)buf)+chpltypeoffset);
  part2 = (int)((*(int64_t *)(((char *)buf)+chpltypeoffset)) >> 32);

  //  PVM_NO_LOCK_SAFE(pvm_pklong((long *)(((char *)buf)+chpltypeoffset), 1, 1), "pvm_pkint", "chpl_pvm_send");
  PVM_NO_LOCK_SAFE(pvm_pkint(&part1, 1, 1), "pvm_pkint", "chpl_pvm_send");
  PVM_NO_LOCK_SAFE(pvm_pkint(&part2, 1, 1), "pvm_pkint", "chpl_pvm_send");
#if CHPL_DIST_DEBUG
  if ((((char *)buf)+chpltypeoffset) != 0) {
    sprintf(debugMsg, "Packing int64_t %d (part %d) of type %d, offset %lu", *(int *)(((char *)buf)+chpltypeoffset), i, chpltypetype, chpltypeoffset);
    PRINTF(debugMsg);
  }
#endif

  return;
}

static void chpl_upkint64_t(void* buf, int i, int chpltypetype, unsigned long chpltypeoffset, int mysize) {
  int part1, part2;
#if CHPL_DIST_DEBUG
  char debugMsg[DEBUG_MSG_LENGTH];
#endif

  PVM_NO_LOCK_SAFE(pvm_upkint(&part1, 1, 1), "pvm_upkint", "chpl_pvm_recv");
  PVM_NO_LOCK_SAFE(pvm_upkint(&part2, 1, 1), "pvm_upkint", "chpl_pvm_recv");
  *(int64_t *)(((char *)buf)+chpltypeoffset) = (int64_t)((((int64_t)part2) << 32) + (((int64_t)part1) & 0x00000000ffffffff));
  //  if (mysize == 8) {
  //    PVM_NO_LOCK_SAFE(pvm_upklong((long *)(((char *)buf)+chpltypeoffset), 1, 1), "pvm_upkint", "chpl_pvm_recv");
#if CHPL_DIST_DEBUG
  sprintf(debugMsg, "Unpacking int64_t %d (part %d) of type %d, offset %lu", *(int *)(((char *)buf)+chpltypeoffset), i, chpltypetype, chpltypeoffset);
  PRINTF(debugMsg);
#endif
  //  } else {
  //    chpl_internal_error("Error: Conversion necessary!");
  //  }
  return;
}
/////////////////////////////////////////////////////////////////////////////

/////////////////////////////////////////////////////////////////////////////
// uint16_t
// No matter what (32->32, 32->64, or 64->32), the sender is just sending
// 16 bits, and the receiver receives 16 bits. No conversions should be
// necessary.
static void chpl_pkuint16_t(void* buf, int i, int chpltypetype, unsigned long chpltypeoffset) {
#if CHPL_DIST_DEBUG
  char debugMsg[DEBUG_MSG_LENGTH];
#endif

  PVM_NO_LOCK_SAFE(pvm_pkushort((uint16_t *)(((char *)buf)+chpltypeoffset), 1, 1), "pvm_pkushort", "chpl_pvm_send");
#if CHPL_DIST_DEBUG
  if ((((char *)buf)+chpltypeoffset) != 0) {
    sprintf(debugMsg, "Packing uint16_t %u (part %d) of type %d, offset %lu", *(unsigned int *)(((char *)buf)+chpltypeoffset), i, chpltypetype, chpltypeoffset);
    PRINTF(debugMsg);
  }
#endif

  return;
}

static void chpl_upkuint16_t(void* buf, int i, int chpltypetype, unsigned long chpltypeoffset) {
#if CHPL_DIST_DEBUG
  char debugMsg[DEBUG_MSG_LENGTH];
#endif

  PVM_NO_LOCK_SAFE(pvm_upkushort((uint16_t *)(((char *)buf)+chpltypeoffset), 1, 1), "pvm_upkushort", "chpl_pvm_recv");
#if CHPL_DIST_DEBUG
  sprintf(debugMsg, "Unpacking uint16_t %u (part %d) of type %d, offset %lu", *(unsigned int *)(((char *)buf)+chpltypeoffset), i, chpltypetype, chpltypeoffset);
  PRINTF(debugMsg);
#endif

  return;
}
/////////////////////////////////////////////////////////////////////////////

/////////////////////////////////////////////////////////////////////////////
// uint32_t
// No matter what (32->32, 32->64, or 64->32), the sender is just sending
// 32 bits, and the receiver receives 32 bits. No conversions should be
// necessary.
static void chpl_pkuint32_t(void* buf, int i, int chpltypetype, unsigned long chpltypeoffset) {
#if CHPL_DIST_DEBUG
  char debugMsg[DEBUG_MSG_LENGTH];
#endif

  PVM_NO_LOCK_SAFE(pvm_pkuint((uint32_t *)(((char *)buf)+chpltypeoffset), 1, 1), "pvm_pkuint", "chpl_pvm_send");
#if CHPL_DIST_DEBUG
  if ((((char *)buf)+chpltypeoffset) != 0) {
    sprintf(debugMsg, "Packing uint32_t %u (part %d) of type %d, offset %lu", *(unsigned int *)(((char *)buf)+chpltypeoffset), i, chpltypetype, chpltypeoffset);
    PRINTF(debugMsg);
  }
#endif

  return;
}

static void chpl_upkuint32_t(void* buf, int i, int chpltypetype, unsigned long chpltypeoffset) {
#if CHPL_DIST_DEBUG
  char debugMsg[DEBUG_MSG_LENGTH];
#endif

  PVM_NO_LOCK_SAFE(pvm_upkuint((uint32_t *)(((char *)buf)+chpltypeoffset), 1, 1), "pvm_upkuint", "chpl_pvm_recv");
#if CHPL_DIST_DEBUG
  sprintf(debugMsg, "Unpacking uint32_t %u (part %d) of type %d, offset %lu", *(unsigned int *)(((char *)buf)+chpltypeoffset), i, chpltypetype, chpltypeoffset);
  PRINTF(debugMsg);
#endif

  return;
}
/////////////////////////////////////////////////////////////////////////////

/////////////////////////////////////////////////////////////////////////////
// uint64_t
// If the receiver is 32-bit, conversion is necessary.
static void chpl_pkuint64_t(void* buf, int i, int chpltypetype, unsigned long chpltypeoffset) {
  int part1, part2;
#if CHPL_DIST_DEBUG
  char debugMsg[DEBUG_MSG_LENGTH];
#endif

  part1 = (int)*(int64_t *)(((char *)buf)+chpltypeoffset);
  part2 = (int)((*(int64_t *)(((char *)buf)+chpltypeoffset)) >> 32);

  //  PVM_NO_LOCK_SAFE(pvm_pkulong((unsigned long *)(((char *)buf)+chpltypeoffset), 1, 1), "pvm_pkuint", "chpl_pvm_send");
  PVM_NO_LOCK_SAFE(pvm_pkint(&part1, 1, 1), "pvm_pkint", "chpl_pvm_send");
  PVM_NO_LOCK_SAFE(pvm_pkint(&part2, 1, 1), "pvm_pkint", "chpl_pvm_send");
#if CHPL_DIST_DEBUG
  if ((((char *)buf)+chpltypeoffset) != 0) {
    sprintf(debugMsg, "Packing uint64_t %u (part %d) of type %d, offset %lu", *(unsigned int *)(((char *)buf)+chpltypeoffset), i, chpltypetype, chpltypeoffset);
    PRINTF(debugMsg);
  }
#endif

  return;
}

static void chpl_upkuint64_t(void* buf, int i, int chpltypetype, unsigned long chpltypeoffset, int mysize) {
  int part1, part2;
#if CHPL_DIST_DEBUG
  char debugMsg[DEBUG_MSG_LENGTH];
#endif

  PVM_NO_LOCK_SAFE(pvm_upkint(&part1, 1, 1), "pvm_upkint", "chpl_pvm_recv");
  PVM_NO_LOCK_SAFE(pvm_upkint(&part2, 1, 1), "pvm_upkint", "chpl_pvm_recv");
  *(int64_t *)(((char *)buf)+chpltypeoffset) = (int64_t)((((int64_t)part2) << 32) + (((int64_t)part1) & 0x00000000ffffffff));
  //  if (mysize == 8) {
  //    PVM_NO_LOCK_SAFE(pvm_upkulong((unsigned long *)(((char *)buf)+chpltypeoffset), 1, 1), "pvm_upkuint", "chpl_pvm_recv");
#if CHPL_DIST_DEBUG
  sprintf(debugMsg, "Unpacking uint64_t %u (part %d) of type %d, offset %lu", *(unsigned int *)(((char *)buf)+chpltypeoffset), i, chpltypetype, chpltypeoffset);
  PRINTF(debugMsg);
#endif
    //  } else {
    //    chpl_internal_error("Error: Conversion necessary!");
    //  }
  return;
}
/////////////////////////////////////////////////////////////////////////////

/////////////////////////////////////////////////////////////////////////////
// real32_t, imag32_t
// No matter what (32->32, 32->64, or 64->32), the sender is just sending
// 32 bits, and the receiver receives 32 bits. No conversions should be
// necessary.
static void chpl_pkfloat32_t(void* buf, int i, int chpltypetype, unsigned long chpltypeoffset) {
#if CHPL_DIST_DEBUG
  char debugMsg[DEBUG_MSG_LENGTH];
#endif

  PVM_NO_LOCK_SAFE(pvm_pkfloat((float *)(((char *)buf)+chpltypeoffset), 1, 1), "pvm_pkfloat", "chpl_pvm_send");
#if CHPL_DIST_DEBUG
  if ((((char *)buf)+chpltypeoffset) != 0) {
    sprintf(debugMsg, "Packing float %f (part %d) of type %d, offset %lu", *(float *)(((char *)buf)+chpltypeoffset), i, chpltypetype, chpltypeoffset);
    PRINTF(debugMsg);
  }
#endif

  return;
}

static void chpl_upkfloat32_t(void* buf, int i, int chpltypetype, unsigned long chpltypeoffset) {
#if CHPL_DIST_DEBUG
  char debugMsg[DEBUG_MSG_LENGTH];
#endif

  PVM_NO_LOCK_SAFE(pvm_upkfloat((float *)(((char *)buf)+chpltypeoffset), 1, 1), "pvm_upkfloat", "chpl_pvm_recv");
#if CHPL_DIST_DEBUG
  sprintf(debugMsg, "Unpacking float %f (part %d) of type %d, offset %lu", *(float *)(((char *)buf)+chpltypeoffset), i, chpltypetype, chpltypeoffset);
  PRINTF(debugMsg);
#endif

  return;
}
/////////////////////////////////////////////////////////////////////////////

/////////////////////////////////////////////////////////////////////////////
// real64_t, imag64_t
// If the receiver is 32-bit, conversion is necessary.
static void chpl_pkdouble64_t(void* buf, int i, int chpltypetype, unsigned long chpltypeoffset) {
#if CHPL_DIST_DEBUG
  char debugMsg[DEBUG_MSG_LENGTH];
#endif

  PVM_NO_LOCK_SAFE(pvm_pkdouble((double *)(((char *)buf)+chpltypeoffset), 1, 1), "pvm_pkdouble", "chpl_pvm_send");
#if CHPL_DIST_DEBUG
  if ((((char *)buf)+chpltypeoffset) != 0) {
    sprintf(debugMsg, "Packing double %f (part %d) of type %d, offset %lu", *(double *)(((char *)buf)+chpltypeoffset), i, chpltypetype, chpltypeoffset);
    PRINTF(debugMsg);
  }
#endif

  return;
}

static void chpl_upkdouble64_t(void* buf, int i, int chpltypetype, unsigned long chpltypeoffset, int mysize) {
#if CHPL_DIST_DEBUG
  char debugMsg[DEBUG_MSG_LENGTH];
#endif

  PVM_NO_LOCK_SAFE(pvm_upkdouble((double *)(((char *)buf)+chpltypeoffset), 1, 1), "pvm_upkdouble", "chpl_pvm_recv");
#if CHPL_DIST_DEBUG
  sprintf(debugMsg, "Unpacking double %f (part %d) of type %d, offset %lu", *(double *)(((char *)buf)+chpltypeoffset), i, chpltypetype, chpltypeoffset);
  PRINTF(debugMsg);
#endif
  return;
}
/////////////////////////////////////////////////////////////////////////////

/////////////////////////////////////////////////////////////////////////////
// CLASS_REFERENCE
// A pointer can be of size 32 or 64 depending on the sender.
// So, if the sender and receiver have the same size, no conversion is
// necessary. If the sender is 32-bits, and the receiver is 64-bits, there
// is no data loss in that the sender can use an int to store into a long.
// If the sender is 64-bits, and the receiver is 32-bits, conversion is
// necessary.
static void chpl_pkCLASS_REFERENCE(void* buf, int i, int chpltypetype, unsigned long chpltypeoffset) {
  int part1, part2;
#if CHPL_DIST_DEBUG
  char debugMsg[DEBUG_MSG_LENGTH];
#endif
  int packagesize = sizeof(void *);
  PVM_NO_LOCK_SAFE(pvm_pkint(&packagesize, 1, 1), "pvm_pkint", "chpl_pvm_send");
  if (packagesize == 4) {
    PVM_NO_LOCK_SAFE(pvm_pkuint((uint32_t *)(((char *)buf)+chpltypeoffset), 1, 1), "pvm_pkuint", "chpl_pvm_send");
#if CHPL_DIST_DEBUG
    sprintf(debugMsg, "Packing CLASS_REFERENCE 0x%x (part %d) of type %d, offset %lu", *(unsigned int *)(((char *)buf)+chpltypeoffset), i, chpltypetype, chpltypeoffset);
    PRINTF(debugMsg);
#endif
    
  } else if (packagesize == 8) {
    part1 = (int)*(int64_t *)(((char *)buf)+chpltypeoffset);
    part2 = (int)((*(int64_t *)(((char *)buf)+chpltypeoffset)) >> 32);

    // pklong doesn't work here since it assumes a long is 32-bits
    //    PVM_NO_LOCK_SAFE(pvm_pklong((long *)(((char *)buf)+chpltypeoffset), 1, 1), "pvm_pklong", "chpl_pvm_send");
    PVM_NO_LOCK_SAFE(pvm_pkint(&part1, 1, 1), "pvm_pkint", "chpl_pvm_send");
    PVM_NO_LOCK_SAFE(pvm_pkint(&part2, 1, 1), "pvm_pkint", "chpl_pvm_send");
#if CHPL_DIST_DEBUG
    sprintf(debugMsg, "Packing CLASS_REFERENCE 0x%lx (part %d) of type %d, offset %lu", *(long unsigned int *)(((char *)buf)+chpltypeoffset), i, chpltypetype, chpltypeoffset);
    PRINTF(debugMsg);
#endif
  } else {
    chpl_internal_error("Error: Unexpected byte size!");
  }
}

static void chpl_upkCLASS_REFERENCE(void* buf, int i, int chpltypetype, unsigned long chpltypeoffset, int mysize) {
  int part1, part2;
#if CHPL_DIST_DEBUG
  char debugMsg[DEBUG_MSG_LENGTH];
#endif
  int packagesize;
  PVM_NO_LOCK_SAFE(pvm_upkint(&packagesize, 1, 1), "pvm_upkint", "chpl_pvm_recv");
  if (packagesize == 4) {
    PVM_NO_LOCK_SAFE(pvm_upkuint((uint32_t *)(((char *)buf)+chpltypeoffset), 1, 1), "pvm_upkuint", "chpl_pvm_recv");
#if CHPL_DIST_DEBUG
    sprintf(debugMsg, "Unpacking CLASS_REFERENCE 0x%x (part %d) of type %d, offset %lu", *(unsigned int *)(((char *)buf)+chpltypeoffset), i, chpltypetype, chpltypeoffset);
    PRINTF(debugMsg);
#endif
  } else if (packagesize == 8) {
    PVM_NO_LOCK_SAFE(pvm_upkint(&part1, 1, 1), "pvm_upkint", "chpl_pvm_recv");
    PVM_NO_LOCK_SAFE(pvm_upkint(&part2, 1, 1), "pvm_upkint", "chpl_pvm_recv");
    *(int64_t *)(((char *)buf)+chpltypeoffset) = (int64_t)((((int64_t)part2) << 32) + (((int64_t)part1) & 0x00000000ffffffff));
    // pklong doesn't work here since it assumes a long is 32-bits
    //    PVM_NO_LOCK_SAFE(pvm_upklong((long *)(((char *)buf)+chpltypeoffset), 1, 1), "pvm_upklong", "chpl_pvm_recv");
#if CHPL_DIST_DEBUG
    sprintf(debugMsg, "Unpacking CLASS_REFERENCE 0x%lx (part %d) of type %d, offset %lu", *(long unsigned int *)(((char *)buf)+chpltypeoffset), i, chpltypetype, chpltypeoffset);
    PRINTF(debugMsg);
#endif
  } else {
    chpl_internal_error("Error: Unexpected byte size!");
  }
}
/////////////////////////////////////////////////////////////////////////////
#endif

// Return the source of the message received.
static int chpl_pvm_recv(int tid, int msgtag, void* buf, int sz) {
  int bufid;
  int bytes;
  int pvmtype;
  int source;
  int repTag;
  int fnid;
  int ack;                      // send an ack to sending node

#ifdef CHPL_COMM_HETEROGENEOUS
  int i;                        // iterates over entries in chpl_structType
  int chpltypetype;             // from enumeration table
  unsigned long chpltypeoffset; // from enumeration table
  int sendingnil;               // for send of (nil)
  int break_out = 0;            // to get out of for from switch
  int32_t convertvar;           // convert var to int32_t for send/recv
  uint32_t uconvertvar;         // convert var to uint32_t for send/recv
#else
  int packagesize;              // remote node says how big something is
#endif

#if CHPL_DIST_DEBUG
  char debugMsg[DEBUG_MSG_LENGTH];
#endif
  if ((tid < chpl_numLocales) && (tid > -1)) tid = tids[tid];   // lines up Chapel locales and PVM index

#if CHPL_DIST_DEBUG
  sprintf(debugMsg, "chpl_pvm_recv(%p, from=%d, tag=%d, sz=%d)", buf, tid, msgtag, sz);
  PRINTF(debugMsg);
#endif
  bufid = 0;
  while (bufid == 0) {
    PVM_PACK_SAFE(bufid = pvm_nrecv(tid, msgtag), "pvm_nrecv", "chpl_pvm_recv");
    if (bufid == 0) {
      CHPL_MUTEX_UNLOCK(&pvm_lock);
    }
  }
  PVM_NO_LOCK_SAFE(pvm_bufinfo(bufid, &bytes, &pvmtype, &source), "pvm_bufinfo", "chpl_pvm_recv");

  // Either getting "metadata" case, which is the chpl_message_info
  // containing the information about what's to come, or getting
  // actual data.
  if (msgtag == TAGMASK+1) {
    PVM_NO_LOCK_SAFE(pvm_upkint(&pvmtype, 1, 1), "pvm_upkint", "chpl_pvm_recv");
    PVM_NO_LOCK_SAFE(pvm_upkint(&repTag, 1, 1), "pvm_upkint", "chpl_pvm_recv");
    PVM_NO_LOCK_SAFE(pvm_upkint(&bytes, 1, 1), "pvm_upkint", "chpl_pvm_recv");
    // Metadata case either contains an address for the data or a
    // function ID (or, if ChplCommFinish, nothing).
    if ((pvmtype == ChplCommPut) || (pvmtype == ChplCommGet)) {
#ifdef CHPL_COMM_HETEROGENEOUS
      chpl_upkCLASS_REFERENCE((void *)&(((_chpl_message_info *)buf)->u.data), 0, CHPL_TYPE_CLASS_REFERENCE, 0, sizeof(void *));
      CHPL_MUTEX_UNLOCK(&pvm_lock);
#else
      PVM_NO_LOCK_SAFE(pvm_upkint(&packagesize, 1, 1), "pvm_upkint", "chpl_pvm_recv");
      PVM_UNPACK_SAFE(pvm_upkbyte((void *)&(((_chpl_message_info *)buf)->u.data), packagesize, 1), "pvm_upkbyte", "chpl_pvm_recv");
#endif
    } else if (pvmtype == ChplCommFinish) {
      // Do nothing. Nothing in the union.
      CHPL_MUTEX_UNLOCK(&pvm_lock);
    } else {
      PVM_UNPACK_SAFE(pvm_upkint(&fnid, 1, 1), "pvm_upkint", "chpl_pvm_recv");
      ((_chpl_message_info *)buf)->u.fid = (chpl_fn_int_t) fnid;
    }
    ((_chpl_message_info *)buf)->msg_type = (int) pvmtype;
    ((_chpl_message_info *)buf)->replyTag = (int) repTag;
    ((_chpl_message_info *)buf)->size = (int) bytes;

  // Getting actual data
  } else {
#ifdef CHPL_COMM_HETEROGENEOUS
    PVM_NO_LOCK_SAFE(pvm_upkint(&sendingnil, 1 ,1), "pvm_upkint", "chpl_pvm_recv");
    if (sendingnil != 1) {
      // Handle types that aren't easy to send via PVM.
      i = 0;
      if (sz > 0) {
        for (; i < chpl_max_fields_per_type; i++) {
          if ((chpl_getFieldType(sz, i) == CHPL_TYPE_wide_string) ||
              (chpl_getFieldType(sz, i) == CHPL_TYPE__cfile) ||
              (chpl_getFieldType(sz, i) == CHPL_TYPE_chpl_task_list_p) ||
              (chpl_getFieldType(sz, i) == CHPL_TYPE__timervalue)) {
            chpl_internal_error("Error: Unimplmented case!");
            PVM_NO_LOCK_SAFE(pvm_upkbyte(((char *)buf), chpl_getFieldSize(sz), 1), "pvm_upkbyte", "chpl_pvm_recv");
#if CHPL_DIST_DEBUG
            sprintf(debugMsg, "Unpacking something");
            PRINTF(debugMsg);
#endif
            i = chpl_max_fields_per_type;
          }
        }
      }
      if (i == chpl_max_fields_per_type) {
        // If i is chpl_max_fields_per_type + 1, we found a type we couldn't
        // use this table for. Thus, we sent it as a collection of bytes, and
        // we short-circuit this table. Otherwise, run through this table
        // normally.
        i = 0;
      }
      for (; i < chpl_max_fields_per_type; i++) {
        // A positive size represents the entry into the chpl_rt_types
        //      enumeration.
        // A negative size represents a direct call for a get (in which
        //      we've sent the chplType enumeration without conversion
        //      after the negative.
        if (sz < 0) {
          if (sz != -CHPL_TYPE_chpl_string) {
            chpltypetype = sz * -1;
          } else {
            chpltypetype = -CHPL_TYPE_chpl_string;
          }
          chpltypeoffset = 0;
          i = chpl_max_fields_per_type;
        }
        else {
          chpltypetype = chpl_getFieldType(sz, i);
          chpltypeoffset = chpl_getFieldOffset(sz, i);
        }
        switch (chpltypetype) {
        case CHPL_TYPE_chpl_bool:
          convertvar = 0;
          PVM_NO_LOCK_SAFE(pvm_upkint(&convertvar, 1, 1), "pvm_upkint", "chpl_pvm_recv");
          *(chpl_bool *)(((char *)buf)+chpltypeoffset) = (chpl_bool)convertvar;
#if CHPL_DIST_DEBUG
          sprintf(debugMsg, "Unpacking %d (part %d) of type %d, offset %lu", *(chpl_bool *)(((char *)buf)+chpltypeoffset), i, chpltypetype, chpltypeoffset);
          PRINTF(debugMsg);
#endif
          break;
        case CHPL_TYPE_int8_t:
          convertvar = 0;
          PVM_NO_LOCK_SAFE(pvm_upkint(&convertvar, 1, 1), "pvm_upkint", "chpl_pvm_recv");
          *(int8_t *)(((char *)buf)+chpltypeoffset) = (int8_t)convertvar;
          //          PVM_NO_LOCK_SAFE(pvm_upkbyte((((char *)buf)+chpltypeoffset), 1, 1), "pvm_upkint", "chpl_pvm_recv");
#if CHPL_DIST_DEBUG
          sprintf(debugMsg, "Unpacking %d (part %d) of type %d, offset %lu", *(int8_t *)(((char *)buf)+chpltypeoffset), i, chpltypetype, chpltypeoffset);
          PRINTF(debugMsg);
#endif
          break;
        case CHPL_TYPE_int16_t:
          chpl_upkint16_t(buf, i, chpltypetype, chpltypeoffset);
          break;
        case CHPL_TYPE_enum:
        case CHPL_TYPE_int32_t:
          chpl_upkint32_t(buf, i, chpltypetype, chpltypeoffset);
          break;
        case CHPL_TYPE_int64_t:
          chpl_upkint64_t(buf, i, chpltypetype, chpltypeoffset, sizeof(void *));
          break;
        case CHPL_TYPE_uint8_t:
          uconvertvar = 0;
          PVM_NO_LOCK_SAFE(pvm_upkuint(&uconvertvar, 1, 1), "pvm_upkuint", "chpl_pvm_recv");
          *(uint8_t *)(((char *)buf)+chpltypeoffset) = (uint8_t)uconvertvar;
          //          PVM_NO_LOCK_SAFE(pvm_upkbyte((((char *)buf)+chpltypeoffset), 1, 1), "pvm_upkint", "chpl_pvm_recv");
#if CHPL_DIST_DEBUG
          sprintf(debugMsg, "Unpacking %u (part %d) of type %d, offset %lu", *(uint8_t *)(((char *)buf)+chpltypeoffset), i, chpltypetype, chpltypeoffset);
          PRINTF(debugMsg);
#endif
          break;
        case CHPL_TYPE_uint16_t:
          chpl_upkuint16_t(buf, i, chpltypetype, chpltypeoffset);
          break;
        case CHPL_TYPE_uint32_t:
          chpl_upkuint32_t(buf, i, chpltypetype, chpltypeoffset);
          break;
        case CHPL_TYPE_uint64_t:
          chpl_upkuint64_t(buf, i, chpltypetype, chpltypeoffset, sizeof(void *));
          break;
        case CHPL_TYPE__real32:
        case CHPL_TYPE__imag32:
          chpl_upkfloat32_t(buf, i, chpltypetype, chpltypeoffset);
          break;
        case CHPL_TYPE__real64:
        case CHPL_TYPE__imag64:
          chpl_upkdouble64_t(buf, i, chpltypetype, chpltypeoffset, sizeof(void *));
          break;
        case CHPL_TYPE__complex64:
          chpl_upkfloat32_t(buf, i, chpltypetype, chpltypeoffset);
          chpl_upkfloat32_t(buf, i, chpltypetype, (chpltypeoffset + sizeof(float)));
          break;
        case CHPL_TYPE__complex128:
          chpl_upkdouble64_t(buf, i, chpltypetype, chpltypeoffset, sizeof(void *));
          chpl_upkdouble64_t(buf, i, chpltypetype, (chpltypeoffset + sizeof(double)), sizeof(void *));
          break;
        case -CHPL_TYPE_chpl_string:
          PVM_NO_LOCK_SAFE(pvm_upkstr(((char *)buf)+chpltypeoffset), "pvm_upkstr", "chpl_pvm_recv");
#if CHPL_DIST_DEBUG
          sprintf(debugMsg, "Unpacking chpl_string %s (part %d) of type %d, offset %lu", (((char *)buf)+chpltypeoffset), i, chpltypetype, chpltypeoffset);
          PRINTF(debugMsg);
#endif
          break;
        case CHPL_TYPE_chpl_string:
        case CHPL_TYPE_CLASS_REFERENCE:
          chpl_upkCLASS_REFERENCE(buf, i, chpltypetype, chpltypeoffset, sizeof(void *));
          break;
        case CHPL_TYPE_DONE:
          break_out = 1;
          break;
        default:
          chpl_internal_error("Error: Unknown case!");
          break;
        }
        if (break_out == 1) {
          break;
        }
      }
    }
    CHPL_MUTEX_UNLOCK(&pvm_lock);
#else
    PVM_UNPACK_SAFE(pvm_upkbyte(buf, sz, 1), "pvm_upkbyte", "chpl_pvm_recv");
#endif
  }

  // Send an ack to the other node
  if (msgtag != TAGMASK+1) {
    ack = 0;
    PVM_PACK_SAFE(pvm_initsend(PvmDataDefault), "pvm_initsend", "chpl_pvm_recv");
    PVM_NO_LOCK_SAFE(pvm_pkint(&ack, 1, 1), "pvm_pkint", "chpl_pvm_recv");
    PVM_UNPACK_SAFE(pvm_send(tid, msgtag), "pvm_send", "chpl_pvm_recv");
  }

#if CHPL_DIST_DEBUG
  sprintf(debugMsg, "chpl_pvm_recv(%p, from=%d, tag=%d, sz=%d) done", buf, tid, msgtag, sz);
  PRINTF(debugMsg);
#endif
  return source;
}

static void chpl_pvm_send(int tid, int msgtag, void* buf, int sz) {
  int bufid;                    // for synchronizing pvm_send at end
  int msgtype;
  int repTag;
  int datasize;
  int fnid;
  int ack;                      // getting nil as ack from remote recv
  
#ifdef CHPL_COMM_HETEROGENEOUS
  int i;                        // iterates over entries in chpl_structType
  int chpltypetype;             // from enumeration table
  unsigned long chpltypeoffset; // from enumeration table
  char *conversion;             // determine (char *)buf+chpltypeoffset value
  int sendingnil;               // for send of (nil)
  int break_out = 0;            // to get out of for from switch
  int32_t convertvar;           // convert var to int32_t for send/recv
  uint32_t uconvertvar;         // convert var to uint32_t for send/recv
#else
  int packagesize;              // tells remote node how big something is
#endif

#if CHPL_DIST_DEBUG
  char debugMsg[DEBUG_MSG_LENGTH];
#endif
  if ((tid < chpl_numLocales) && (tid > -1)) tid = tids[tid];  // lines up Chapel locales and PVM index
#if CHPL_DIST_DEBUG
  sprintf(debugMsg, "chpl_pvm_send(%p, to=%d, tag=%d, sz=%d)", buf, tid, msgtag, sz);
  PRINTF(debugMsg);
#endif
  PVM_PACK_SAFE(pvm_initsend(PvmDataDefault), "pvm_initsend", "chpl_pvm_send");

  // Either sending "metadata" case, which is the chpl_message_info
  // containing the information about what's to come, or sending
  // actual data.
  if (msgtag == TAGMASK+1) {
    msgtype = ((_chpl_message_info *)buf)->msg_type;
    repTag = ((_chpl_message_info *)buf)->replyTag;
    datasize = ((_chpl_message_info *)buf)->size;
    PVM_NO_LOCK_SAFE(pvm_pkint(&msgtype, 1, 1), "pvm_pkint", "chpl_pvm_send");
    PVM_NO_LOCK_SAFE(pvm_pkint(&repTag, 1, 1), "pvm_pkint", "chpl_pvm_send");
    PVM_NO_LOCK_SAFE(pvm_pkint(&datasize, 1, 1), "pvm_pkint", "chpl_pvm_send");
    // Metadata case either contains an address for the data or a
    // function ID (or, if ChplCommFinish, nothing).
    if ((msgtype == ChplCommPut) || (msgtype == ChplCommGet)) {
#ifdef CHPL_COMM_HETEROGENEOUS
      chpl_pkCLASS_REFERENCE((void *)&(((_chpl_message_info *)buf)->u.data), 0, CHPL_TYPE_CLASS_REFERENCE, 0);
#else
      packagesize = sizeof(void *);
      PVM_NO_LOCK_SAFE(pvm_pkint(&packagesize, 1, 1), "pvm_pkint", "chpl_pvm_send");
      PVM_NO_LOCK_SAFE(pvm_pkbyte((void *)&(((_chpl_message_info *)buf)->u.data), packagesize, 1), "pvm_pkbyte", "chpl_pvm_send");
#endif
    } else if (msgtype == ChplCommFinish) {
      // Do nothing. Nothing in the union.
      // Unlock is done in the PVM_UNPACK_SAFE on the actual pvm_send.
    } else {
      fnid = ((_chpl_message_info *)buf)->u.fid;
      PVM_NO_LOCK_SAFE(pvm_pkint(&fnid, 1, 1), "pvm_pkint", "chpl_pvm_send");
    }
  // Sending actual data
  } else {
#ifdef CHPL_COMM_HETEROGENEOUS
    conversion = (char *)buf;
    if (conversion == NULL) {
      sendingnil = 1;
    } else {
      sendingnil = 0;
    }
    PVM_NO_LOCK_SAFE(pvm_pkint(&sendingnil, 1 ,1), "pvm_pkint", "chpl_pvm_send");
    if (sendingnil == 0) {
      // Handle types that aren't easy to send via PVM.
      i = 0;
      if (sz > 0) {
        for (; i < chpl_max_fields_per_type; i++) {
          if ((chpl_getFieldType(sz, i) == CHPL_TYPE_wide_string) ||
              (chpl_getFieldType(sz, i) == CHPL_TYPE__cfile) ||
              (chpl_getFieldType(sz, i) == CHPL_TYPE_chpl_task_list_p) ||
              (chpl_getFieldType(sz, i) == CHPL_TYPE__timervalue)) {
            chpl_internal_error("Error: Unimplmented case!");
            PVM_NO_LOCK_SAFE(pvm_pkbyte(((char *)buf), chpl_getFieldSize(sz), 1), "pvm_pkbyte", "chpl_pvm_send");
#if CHPL_DIST_DEBUG
            sprintf(debugMsg, "Packing something");
            PRINTF(debugMsg);
#endif
            i = chpl_max_fields_per_type;
          }
        }
      }
      if (i == chpl_max_fields_per_type) {
        // If i is chpl_max_fields_per_type + 1, we found a type we couldn't
        // use this table for. Thus, we sent it as a collection of bytes, and
        // we short-circuit this table. Otherwise, run through this table
        // normally.
        i = 0;
      }
      for (; i < chpl_max_fields_per_type; i++) {
        // A positive size represents the entry into the chpl_rt_types
        //      enumeration.
        // A negative size represents a direct call for a get (in which
        //      we've sent the chplType enumeration without conversion
        //      after the negative.
        if (sz < 0) {
          if (sz != -CHPL_TYPE_chpl_string) {
            chpltypetype = sz * -1;
          } else {
            chpltypetype = -CHPL_TYPE_chpl_string;
          }
          chpltypeoffset = 0;
          i = chpl_max_fields_per_type;
        }
        else {
          chpltypetype = chpl_getFieldType(sz, i);
          chpltypeoffset = chpl_getFieldOffset(sz, i);
        }
        switch (chpltypetype) {
        case CHPL_TYPE_chpl_bool:
          convertvar = (int32_t)*(chpl_bool *)(((char *)buf)+chpltypeoffset);
          PVM_NO_LOCK_SAFE(pvm_pkint(&convertvar, 1, 1), "pvm_pkint", "chpl_pvm_send");
#if CHPL_DIST_DEBUG
          sprintf(debugMsg, "Packing %d (part %d) of type %d, offset %lu", *(chpl_bool *)(((char *)buf)+chpltypeoffset), i, chpltypetype, chpltypeoffset);
          PRINTF(debugMsg);
#endif
          break;
        case CHPL_TYPE_int8_t:
          convertvar = (int32_t)*(int8_t *)(((char *)buf)+chpltypeoffset);
          PVM_NO_LOCK_SAFE(pvm_pkint(&convertvar, 1, 1), "pvm_pkint", "chpl_pvm_send");
          //          PVM_NO_LOCK_SAFE(pvm_pkbyte((((char *)buf)+chpltypeoffset), 1, 1), "pvm_pkbyte", "chpl_pvm_send");
#if CHPL_DIST_DEBUG
          sprintf(debugMsg, "Packing %d (part %d) of type %d, offset %lu", *(int8_t *)(((char *)buf)+chpltypeoffset), i, chpltypetype, chpltypeoffset);
          PRINTF(debugMsg);
#endif
          break;
        case CHPL_TYPE_int16_t:
          chpl_pkint16_t(buf, i, chpltypetype, chpltypeoffset);
          break;
        case CHPL_TYPE_enum:
        case CHPL_TYPE_int32_t:
          chpl_pkint32_t(buf, i, chpltypetype, chpltypeoffset);
          break;
        case CHPL_TYPE_int64_t:
          chpl_pkint64_t(buf, i, chpltypetype, chpltypeoffset);
          break;
        case CHPL_TYPE_uint8_t:
          uconvertvar = (uint32_t)*(uint8_t *)(((char *)buf)+chpltypeoffset);
          PVM_NO_LOCK_SAFE(pvm_pkuint(&uconvertvar, 1, 1), "pvm_pkuint", "chpl_pvm_send");
          //          PVM_NO_LOCK_SAFE(pvm_pkbyte((((char *)buf)+chpltypeoffset), 1, 1), "pvm_pkbyte", "chpl_pvm_send");
#if CHPL_DIST_DEBUG
          sprintf(debugMsg, "Packing %u (part %d) of type %d, offset %lu", *(uint8_t *)(((char *)buf)+chpltypeoffset), i, chpltypetype, chpltypeoffset);
          PRINTF(debugMsg);
#endif
          break;
        case CHPL_TYPE_uint16_t:
          chpl_pkuint16_t(buf, i, chpltypetype, chpltypeoffset);
          break;
        case CHPL_TYPE_uint32_t:
          chpl_pkuint32_t(buf, i, chpltypetype, chpltypeoffset);
          break;
        case CHPL_TYPE_uint64_t:
          chpl_pkuint64_t(buf, i, chpltypetype, chpltypeoffset);
          break;
        case CHPL_TYPE__real32:
        case CHPL_TYPE__imag32:
          chpl_pkfloat32_t(buf, i, chpltypetype, chpltypeoffset);
          break;
        case CHPL_TYPE__real64:
        case CHPL_TYPE__imag64:
          chpl_pkdouble64_t(buf, i, chpltypetype, chpltypeoffset);
          break;
        case CHPL_TYPE__complex64:
          chpl_pkfloat32_t(buf, i, chpltypetype, chpltypeoffset);
          chpl_pkfloat32_t(buf, i, chpltypetype, (chpltypeoffset + sizeof(float)));
          break;
        case CHPL_TYPE__complex128:
          chpl_pkdouble64_t(buf, i, chpltypetype, chpltypeoffset);
          chpl_pkdouble64_t(buf, i, chpltypetype, (chpltypeoffset + sizeof(double)));
          break;
        case -CHPL_TYPE_chpl_string:
          PVM_NO_LOCK_SAFE(pvm_pkstr(((char *)buf)+chpltypeoffset), "pvm_pkstr", "chpl_pvm_send");
#if CHPL_DIST_DEBUG
          sprintf(debugMsg, "Packing chpl_string %s (part %d) of type %d, offset %lu", (((char *)buf)+chpltypeoffset), i, chpltypetype, chpltypeoffset);
          PRINTF(debugMsg);
#endif
          break;
        case CHPL_TYPE_chpl_string:
        case CHPL_TYPE_CLASS_REFERENCE:
          chpl_pkCLASS_REFERENCE(buf, i, chpltypetype, chpltypeoffset);
          break;
        case CHPL_TYPE_DONE:
          break_out = 1;
          break;
        default:
          chpl_internal_error("Error: Unknown case!");
          break;
        }
        if (break_out == 1) {
          break;
        }
      }
    }
#else
    PVM_NO_LOCK_SAFE(pvm_pkbyte(buf, sz, 1), "pvm_pkbyte", "chpl_pvm_send");
#endif
  }
  PVM_UNPACK_SAFE(pvm_send(tid, msgtag), "pvm_pksend", "chpl_pvm_send");

  // Receive an ack from other node
  if (msgtag != TAGMASK+1) {
    bufid = 0;
    while (bufid == 0) {
      PVM_PACK_SAFE(bufid = pvm_nrecv(tid, msgtag), "pvm_nrecv", "chpl_pvm_send");
      if (bufid == 0) {
        CHPL_MUTEX_UNLOCK(&pvm_lock);
      }
    }
    PVM_UNPACK_SAFE(pvm_upkint(&ack, 1, 1), "pvm_upkbyte", "chpl_pvm_send");
  }

#if CHPL_DIST_DEBUG
  sprintf(debugMsg, "chpl_pvm_send(%p, to=%d, tag=%d, sz=%d) done", buf, tid, msgtag, sz);
  PRINTF(debugMsg);
#endif
  return;
}

static void polling(void* x) {
  int finished;
  int source;
  _chpl_message_info msg_info;
#if CHPL_DIST_DEBUG
  char debugMsg[DEBUG_MSG_LENGTH];
#endif

  int mallocsize;

  PRINTF("Starting PVM polling thread");
  CHPL_MUTEX_LOCK(&termination_lock);
  finished = 0;

  pthread_mutex_lock(&okay_mutex);
  while (okaypoll == 0) {
    pthread_cond_wait(&okay_to_poll, &okay_mutex);
  }
  pthread_mutex_unlock(&okay_mutex);

  while (!finished) {
    PRINTF("Poller Receiving");
    // Poller thread waits for input from anyone
    // First signal is metadata case (TAGMASK+1). Contains information
    // about the forthcoming data (or how the node should respond).
    source = chpl_pvm_recv(-1, TAGMASK+1, &msg_info, sizeof(_chpl_message_info));
    switch (msg_info.msg_type) {
      // ChplCommPut tells node to store forthcoming data into a location.
    case ChplCommPut: {
#if CHPL_DIST_DEBUG
      sprintf(debugMsg, "Fulfilling ChplCommPut(data=%p, size=%d, from=%d, tag=%d)", msg_info.u.data, (int)msg_info.size, source, msg_info.replyTag);
      PRINTF(debugMsg);
#endif
      chpl_pvm_recv(source, msg_info.replyTag, msg_info.u.data, (int)msg_info.size);
      break;
    }
      // ChplCommGet tells node to send data to a location.
    case ChplCommGet: {
#if CHPL_DIST_DEBUG
      sprintf(debugMsg, "Fulfilling ChplCommGet(data=%p, size=%d, to=%d, tag=%d", msg_info.u.data, (int)msg_info.size, source, msg_info.replyTag);
      PRINTF(debugMsg);
#endif
      chpl_pvm_send(source, msg_info.replyTag, msg_info.u.data, (int)msg_info.size);
      break;
    }
      // ChplCommFork gets a function ID and a set of arguments. Non-blocking
      // fork works similarly, but runs it from polling thread.
    case ChplCommFork: {
      void* args;
      _chpl_RPC_arg* rpcArg = chpl_malloc(1, sizeof(_chpl_RPC_arg), CHPL_RT_MD_REMOTE_FORK_DATA, 0, 0);
#if CHPL_DIST_DEBUG
      sprintf(debugMsg, "Fulfilling ChplCommFork(fromloc=%d, tag=%d, fnid=%d)", source, msg_info.replyTag, msg_info.u.fid);
      PRINTF(debugMsg);
#endif

#ifdef CHPL_COMM_HETEROGENEOUS
      mallocsize = chpl_getFieldSize(msg_info.size);
#else
      mallocsize = msg_info.size;
#endif
      if (mallocsize != 0) {
        args = chpl_malloc(1, mallocsize, CHPL_RT_MD_REMOTE_FORK_ARG, 0, 0);
      } else {
        args = NULL;
      }

      chpl_pvm_recv(source, msg_info.replyTag, args, msg_info.size);

      rpcArg->fid = (chpl_fn_int_t)msg_info.u.fid;
      rpcArg->arg = args;
      rpcArg->replyTag = msg_info.replyTag;
      rpcArg->joinLocale = source;

      CHPL_BEGIN((chpl_fn_p)chpl_RPC, rpcArg, true, false, NULL);
      break;
    }
    case ChplCommForkNB: {
      void* args;
#if CHPL_DIST_DEBUG
      sprintf(debugMsg, "Fulfilling ChplCommForkNB(fromloc=%d, tag=%d)", source, msg_info.replyTag);
      PRINTF(debugMsg);
#endif

#ifdef CHPL_COMM_HETEROGENEOUS
      mallocsize = chpl_getFieldSize(msg_info.size);
#else
      mallocsize = msg_info.size;
#endif
      if (mallocsize != 0) {
        args = chpl_malloc(1, mallocsize, CHPL_RT_MD_REMOTE_FORK_ARG, 0, 0);
      } else {
        args = NULL;
      }

      chpl_pvm_recv(source, msg_info.replyTag, args, msg_info.size);
      CHPL_BEGIN((chpl_fn_p)chpl_ftable[msg_info.u.fid], args, true, false, NULL);

      break;
    }
    case ChplCommFinish: {
      PRINTF("ChplCommFinish\n");
      fflush(stdout);
      fflush(stderr);
      CHPL_MUTEX_UNLOCK(&termination_lock);
      pthread_mutex_destroy(&okay_mutex);
      pthread_cond_destroy(&okay_to_poll);
      finished = 1;
      break;
    }
    case ChplCommBroadcastPrivate: {
#if CHPL_DIST_DEBUG
      sprintf(debugMsg, "Fulfilling ChplCommBroadcastPrivate(fid=%d, size=%d, from=%d, tag=%d)", msg_info.u.fid, (int)msg_info.size, source, msg_info.replyTag);
      PRINTF(debugMsg);
#endif
      chpl_pvm_recv(source, msg_info.replyTag, chpl_private_broadcast_table[msg_info.u.fid], (int)msg_info.size);
      break;
    }
    default: {
      chpl_internal_error("Error: default case should never get reached");
      finished = 1;
      break;
    }
    }
  }
}

void chpl_comm_init(int *argc_p, char ***argv_p) {
  pthread_t polling_thread;
  int status;
  int i;
  int max;

#if CHPL_DIST_DEBUG
  char debugMsg[DEBUG_MSG_LENGTH];
#endif

  // Initialize locks
  CHPL_MUTEX_INIT(&pvm_lock);
  CHPL_MUTEX_INIT(&termination_lock);

  // Figure out who spawned this thread (if no one, this will be -23).
  // Still need to lock call, but since -23 is perfectly okay, don't fail.
  PVM_SAFE_OKAY_TO_FAIL(parent = pvm_parent(), "pvm_parent", "chpl_comm_init");
  if ((parent < 0) && (parent != -23)) {
#if CHPL_DIST_DEBUG
    sprintf(debugMsg, "\n\n%d PVM call failed.\n\n", (int)pthread_self());
    chpl_error(debugMsg, 0, 0);
#endif
    chpl_internal_error("PVM call failed.");
  }

  // Figure out how many nodes there are
  chpl_numLocales = (int32_t)atoi((*argv_p)[*argc_p - 1]);

  // Join the group of all nodes (named "job")
  // Barrier until everyone (numLocales) has joined
  // Make sure the chpl_localeID lines up with the join order
  PVM_SAFE(chpl_localeID = pvm_joingroup((char *)"job"), "pvm_joingroup", "chpl_comm_init");
  chpl_comm_barrier("Waiting for all tasks to join group.");

  // Figure out who everyone is
  PVM_SAFE(max = pvm_gsize((char *)"job"), "pvm_gsize", "chpl_comm_init");
  for (i=0; i < max; i++) {
    PVM_SAFE(tids[i] = pvm_gettid((char *)"job", i), "pvm_gettid", "chpl_comm_init");
  }
  
#if CHPL_DIST_DEBUG
  sprintf(debugMsg, "Starting with chpl_numRealms=%d, chpl_numLocales=%d. I am chpl_localeID=%d, or tid=%d", chpl_numRealms, chpl_numLocales, chpl_localeID, tids[chpl_localeID]);
  PRINTF(debugMsg);
#endif

  // Create the pthread to do the work.
  status = pthread_create(&polling_thread, NULL, (void*(*)(void*))polling, 0);
  if (status)
    chpl_internal_error("unable to start polling thread for PVM");
  pthread_detach(polling_thread);

  // Drop the last argument: the numLocales from the launcher.
  // It confuses parseArgs, and we've already captured it.
  *argc_p = *argc_p - 1;
  return;
}

//
// No support for gdb for now
//
static int mysystem(const char* command, const char* description, int ignorestatus) {
  int status;
  int bufid = 0;
  int me = tids[chpl_localeID];
  if (parent >= 0) {
    commsig = 4;

    PVM_PACK_SAFE(pvm_initsend(PvmDataDefault), "pvm_initsend", "mysystem");
    PVM_NO_LOCK_SAFE(pvm_pkint(&commsig, 1, 1), "pvm_pkint", "mysystem");
    PVM_NO_LOCK_SAFE(pvm_pkint(&me, 1, 1), "pvm_pkint", "mysystem");
    PVM_NO_LOCK_SAFE(pvm_pkstr((char *)command), "pvm_pkstr", "mysystem");
    PVM_NO_LOCK_SAFE(pvm_pkstr((char *)description), "pvm_pkstr", "mysystem");
    PVM_NO_LOCK_SAFE(pvm_pkint(&ignorestatus, 1, 1), "pvm_pkint", "mysystem");
    PVM_UNPACK_SAFE(pvm_send(parent, NOTIFYTAG), "pvm_pksend", "mysystem");

    commsig = 0;

    while (bufid == 0) {
      PVM_PACK_SAFE(bufid = pvm_nrecv(parent, NOTIFYTAG), "pvm_nrecv", "mysystem");
      if (bufid == 0) {
        CHPL_MUTEX_UNLOCK(&pvm_lock);
      }
    }
    PVM_UNPACK_SAFE(pvm_upkint(&status, 1, 1), "pvm_upkint", "mysystem");
  } else {

    status = system(command);
    
    if (status == -1) {
      chpl_error("system() fork failed", 0, "(command-line)");
    } else if (status != 0 && !ignorestatus) {
      chpl_error(description, 0, "(command-line)");
    }
    
  }
  return status;
}

int chpl_comm_run_in_gdb(int argc, char* argv[], int gdbArgnum, int* status) {
  int i;
  char numlocstr[128];
  char *command = chpl_glom_strings(2, "gdb -q -ex 'break gdbShouldBreakHere' --args ", argv[0]);

  // Add the number of locales to end of string as per launcher
  sprintf(numlocstr, "%d", chpl_numLocales);
  argc = argc + 1;
  argv[argc-1] = numlocstr;
  argv[argc] = NULL;
  for (i=1; i<argc; i++) {
    if (i != gdbArgnum) {
      command = chpl_glom_strings(3, command, " ", argv[i]);
    }
  }

  fprintf(stderr, "command is: %s\n", command);
  *status = mysystem(command, "running gdb", 0);

  return 1;
}

void chpl_comm_rollcall(void) {
  CHPL_MUTEX_INIT(&chpl_comm_diagnostics_lock);
  chpl_msg(2, "executing on locale %d of %d locale(s): %s\n", chpl_localeID, chpl_numLocales, chpl_localeName());
  // If just one locale, skip the barrier setup
  if (chpl_numLocales != 1) {
    okay_to_barrier = 0;
  }
  okaypoll = 1;
  pthread_mutex_lock(&okay_mutex);
  pthread_cond_signal(&okay_to_poll);
  pthread_mutex_unlock(&okay_mutex);
  return;
}

void chpl_comm_init_shared_heap(void) {
  chpl_initHeap(NULL, 0);
  return;
}

void chpl_comm_alloc_registry(int numGlobals) {
  chpl_globals_registry = chpl_globals_registry_static;
  return;
}

void chpl_comm_broadcast_global_vars(int numGlobals) {
  int i;

#ifndef CHPL_COMM_HETEROGENEOUS
  int size;
#endif

#if CHPL_DIST_DEBUG
  char debugMsg[DEBUG_MSG_LENGTH];
#endif

  PRINTF("start broadcast globals");
  if (chpl_numLocales == 1) {
    return;
  }
  for (i = 0; i < numGlobals; i++) {
    // Either the root node broadcasting with pvm_bcast, or we're one of
    // the slave nodes getting the data.
    if (chpl_localeID == 0) {
#if CHPL_DIST_DEBUG
      sprintf(debugMsg, "Packing chpl_globals_registry[%d] %p", i, chpl_globals_registry[i]);
      PRINTF(debugMsg);
#endif
      PVM_PACK_SAFE(pvm_initsend(PvmDataDefault), "pvm_initsend", "chpl_comm_broadcast_global_vars");
#ifdef CHPL_COMM_HETEROGENEOUS
      chpl_pkCLASS_REFERENCE(chpl_globals_registry[i], 0, CHPL_TYPE_CLASS_REFERENCE, 0);
#else
      size = sizeof(void *);
      PVM_NO_LOCK_SAFE(pvm_pkint(&size, 1, 1), "pvm_pkint", "chpl_comm_broadcast_global_vars");
      PVM_NO_LOCK_SAFE(pvm_pkbyte((char *)chpl_globals_registry[i], size, 1), "pvm_pkbyte", "chpl_comm_broadcast_global_vars");
#endif
      PVM_UNPACK_SAFE(pvm_bcast((char *)"job", BCASTTAG), "pvm_bcast", "chpl_comm_broadcast_global_vars");
    } else {
      PVM_PACK_SAFE(pvm_recv(-1, BCASTTAG), "pvm_recv", "chpl_comm_broadcast_global_vars");
#ifdef CHPL_COMM_HETEROGENEOUS
      chpl_upkCLASS_REFERENCE(chpl_globals_registry[i], 0, CHPL_TYPE_CLASS_REFERENCE, 0, sizeof(void *));
      CHPL_MUTEX_UNLOCK(&pvm_lock);
#else
      PVM_NO_LOCK_SAFE(pvm_upkint(&size, 1, 1), "pvm_upkint", "chpl_comm_broadcast_global_vars");
      PVM_UNPACK_SAFE(pvm_upkbyte((char *)(chpl_globals_registry[i]), size, 1), "pvm_upkbyte", "chpl_comm_broadcast_global_vars");
#endif
#if CHPL_DIST_DEBUG
      sprintf(debugMsg, "Unpacking chpl_globals_registry[%d] %p", i, chpl_globals_registry[i]);
      PRINTF(debugMsg);
#endif
    }
  }
  PRINTF("end broadcast globals");
  return;
}

void chpl_comm_broadcast_private(int id, int size) {
  int i;
  int tag;
  _chpl_message_info msg_info;

#if CHPL_DIST_DEBUG
    char debugMsg[DEBUG_MSG_LENGTH];
#endif

  if (chpl_numLocales == 1) {
    return;
  }

  // Note: this isn't a PVM call, but we need this locked to make
  // sure PVM tags are unique.
  PVM_SAFE(tag = makeTag((int)pthread_self, chpl_localeID), "makeTag", "chpl_comm_broadcast_private");

#if CHPL_DIST_DEBUG
  sprintf(debugMsg, "chpl_comm_broadcast_private(loc=(%d, %p), index=%d, size=%d, tag=%d)", chpl_localeID, &(chpl_private_broadcast_table[id]), id, size, tag);
  PRINTF(debugMsg);
#endif

  // Overloading fid as the index into the chpl_private_broadcast_table.
  msg_info.msg_type = ChplCommBroadcastPrivate;
  msg_info.replyTag = tag;
  msg_info.size = size;
  msg_info.u.fid = id;

  for (i = 0; i < chpl_numLocales; i++) {
    if (i != chpl_localeID) {
      chpl_pvm_send(i, TAGMASK+1, &msg_info, sizeof(_chpl_message_info));
      chpl_pvm_send(i, tag, chpl_private_broadcast_table[id], size);
    }
  }

#if CHPL_DIST_DEBUG
  sprintf(debugMsg, "chpl_comm_broadcast_private(loc=(%d, %p), index=%d, size=%d, tag=%d) done", chpl_localeID, &(chpl_private_broadcast_table[id]), id, size, tag);
  PRINTF(debugMsg);
#endif

  return;
}

void chpl_comm_barrier(const char *msg) {
  int bufid = 0;
  PRINTF(msg);

  // Exit is a funny case, and most of the barrier case is built up around it.
  // Node 0 ignores the barrier before the exit and tears down its threads
  // (which have finished), and calls chpl_comm_exit_all. The rest of the
  // nodes wait for a signal from node 0 that it's ready for them.

  // The signal is okay_to_barrier.

  // okay_to_barrier starts out true, but once all the setup is complete,
  // okay_to_barrier is set to false (done in chpl_comm_rollcall). This
  // safegaurd is important because chpl_comm_barrier is blocking, not
  // thread-safe, and it holds the pvm_lock.

  // Once node 0 tells everyone it's okay to barrier, everyone does the
  // barrier (including node 0 -- done in chpl_comm_exit_all), and proceeds
  // to the termination.
  if (!(strcmp(msg, "chpl_comm_exit_all")) && chpl_localeID == 0) {
    return;
  }
  if (!(strcmp(msg, "barrier before main"))) {
    // Accounts for the barrier before the main loop.
    PVM_SAFE(pvm_barrier((char *)"job", chpl_numLocales), "pvm_barrier", "chpl_comm_barrier");
    return;
  }
  if (!okay_to_barrier) {
    PRINTF("Entering okay_to_barrier loop");
  }
  while (!okay_to_barrier) {
    while (bufid == 0) {
      PVM_PACK_SAFE(bufid = pvm_nrecv(-1, BCASTTAG), "pvm_nrecv", "chpl_comm_barrier");
      if (bufid == 0) {
        CHPL_MUTEX_UNLOCK(&pvm_lock);
      }
    }
    PVM_UNPACK_SAFE(pvm_upkint(&okay_to_barrier, 1, 1), "pvm_upkint", "chpl_comm_barrier");
  }

  if (okay_to_barrier == 2) {
    // This comes from chpl_comm_exit_any from a node that's not 0.
    // If this is the case, node 0 is cranking along in main, and
    // everyone else is waiting here. Let all these nodes exit, but
    // note that chpl_comm_exit_any() tells node 0 to stop polling (and
    // hence exit).
    PVM_SAFE(pvm_barrier((char *)"job", (chpl_numLocales - 1)), "pvm_barrier", "chpl_comm_barrier");
  } else {
    PVM_SAFE(pvm_barrier((char *)"job", chpl_numLocales), "pvm_barrier", "chpl_comm_barrier");
  }

  return;
}

void chpl_comm_exit_all(int status) {
  _chpl_message_info msg_info;
  PRINTF("chpl_comm_exit_all called");
  // Matches code in chpl_comm_barrier. Node 0, on exit, needs to signal
  // to everyone that it's okay to barrier (and thus exit).

  // This line should be entirely moot (never unset if chpl_numLocales is 1).
  if (chpl_numLocales == 1) {
    okay_to_barrier = 1;
  }
  else if (chpl_localeID == 0) {
    okay_to_barrier = 1;
    PVM_PACK_SAFE(pvm_initsend(PvmDataDefault), "pvm_initsend", "chpl_comm_exit_all");
    PVM_NO_LOCK_SAFE(pvm_pkint(&okay_to_barrier, 1, 1), "pvm_pkint", "chpl_comm_exit_all");
    PVM_UNPACK_SAFE(pvm_bcast((char *)"job", BCASTTAG), "pvm_bcast", "chpl_comm_exit_all");
    // Do a matching barrier to everyone still in chpl_comm_barrier.
    PVM_SAFE(pvm_barrier((char *)"job", chpl_numLocales), "pvm_barrier", "chpl_comm_exit_all");
  }
  msg_info.msg_type = ChplCommFinish;
  chpl_pvm_send(chpl_localeID, TAGMASK+1, &msg_info, sizeof(_chpl_message_info));
  PRINTF("Sent shutdown message.");
  CHPL_MUTEX_LOCK(&termination_lock);
  CHPL_MUTEX_UNLOCK(&termination_lock);
  chpl_comm_barrier("About to finalize");

  PVM_SAFE(pvm_lvgroup((char *)"job"), "pvm_lvgroup", "chpl_comm_exit_all");
  // Send a signal back to the launcher that we're done.
  commsig = 1;
  if (parent >= 0) {
    PVM_PACK_SAFE(pvm_initsend(PvmDataDefault), "pvm_initsend", "chpl_comm_exit_all");
    PVM_NO_LOCK_SAFE(pvm_pkint(&commsig, 1, 1), "pvm_pkint", "chpl_comm_exit_all");
    PVM_UNPACK_SAFE(pvm_send(parent, NOTIFYTAG), "pvm_pksend", "chpl_comm_exit_all");
  }

  pvm_exit();
  return;
}

void chpl_comm_exit_any(int status) {
  _chpl_message_info msg_info;
  PRINTF("chpl_comm_exit_any called");
  // Matches code in chpl_comm_barrier. Node 0, on exit, needs to signal
  // to everyone that it's okay to barrier (and thus exit).

  // This line should be entirely moot (never unset if chpl_numLocales is 1).
  if (chpl_numLocales == 1) {
    okay_to_barrier = 1;
  }
  else if (chpl_localeID == 0) {
    okay_to_barrier = 1;
    PVM_PACK_SAFE(pvm_initsend(PvmDataDefault), "pvm_initsend", "chpl_comm_exit_all");
    PVM_NO_LOCK_SAFE(pvm_pkint(&okay_to_barrier, 1, 1), "pvm_pkint", "chpl_comm_exit_all");
    PVM_UNPACK_SAFE(pvm_bcast((char *)"job", BCASTTAG), "pvm_bcast", "chpl_comm_exit_all");
    // Do a matching barrier to everyone still in chpl_comm_barrier.
    PVM_SAFE(pvm_barrier((char *)"job", chpl_numLocales), "pvm_barrier", "chpl_comm_exit_all");
  } else {
    okay_to_barrier = 2;
    PVM_PACK_SAFE(pvm_initsend(PvmDataDefault), "pvm_initsend", "chpl_comm_exit_all");
    PVM_NO_LOCK_SAFE(pvm_pkint(&okay_to_barrier, 1, 1), "pvm_pkint", "chpl_comm_exit_all");
    PVM_UNPACK_SAFE(pvm_bcast((char *)"job", BCASTTAG), "pvm_bcast", "chpl_comm_exit_all");
  }
  msg_info.msg_type = ChplCommFinish;
  chpl_pvm_send(chpl_localeID, TAGMASK+1, &msg_info, sizeof(_chpl_message_info));
  PRINTF("Sent shutdown message.");
  CHPL_MUTEX_LOCK(&termination_lock);
  CHPL_MUTEX_UNLOCK(&termination_lock);
  chpl_comm_barrier("About to finalize");

  PVM_SAFE(pvm_lvgroup((char *)"job"), "pvm_lvgroup", "chpl_comm_exit_all");
  // Send a signal back to the launcher that we're done.
  commsig = 1;
  if (parent >= 0) {
    PVM_PACK_SAFE(pvm_initsend(PvmDataDefault), "pvm_initsend", "chpl_comm_exit_all");
    PVM_NO_LOCK_SAFE(pvm_pkint(&commsig, 1, 1), "pvm_pkint", "chpl_comm_exit_all");
    PVM_UNPACK_SAFE(pvm_send(parent, NOTIFYTAG), "pvm_pksend", "chpl_comm_exit_all");
  }

  pvm_exit();
  return;
}

void chpl_comm_put(void* addr, void* rref, int32_t size, int ln, chpl_string fn) {
  int32_t locale = ((chpl_wide_voidStar *)rref)->locale;
  void* raddr = ((chpl_wide_voidStar *)rref)->addr;
  if (chpl_localeID == locale) {
    if (size < 0) {
      chpl_internal_error("memmove error");
    }
#ifdef CHPL_COMM_HETEROGENEOUS
    memmove(raddr, addr, chpl_getFieldSize(size));
#else
    memmove(raddr, addr, size);
#endif
  } else {
    _chpl_message_info msg_info;
    int tag;

#if CHPL_DIST_DEBUG
    char debugMsg[DEBUG_MSG_LENGTH];
#endif

    if (chpl_comm_diagnostics && !chpl_comm_no_debug_private) {
      CHPL_MUTEX_LOCK(&chpl_comm_diagnostics_lock);
      chpl_comm_puts++;
      CHPL_MUTEX_UNLOCK(&chpl_comm_diagnostics_lock);
    }

    // Note: this isn't a PVM call, but we need this locked to make
    // sure PVM tags are unique.
    PVM_SAFE(tag = makeTag((int)pthread_self, chpl_localeID), "makeTag", "chpl_comm_put");

#if CHPL_DIST_DEBUG
    sprintf(debugMsg, "chpl_comm_put(loc=(%d, %p), rem=(%d, %p), size=%d, tag=%d)", chpl_localeID, addr, locale, raddr, size, tag);
    PRINTF(debugMsg);
#endif
    
    msg_info.msg_type = ChplCommPut;
    msg_info.replyTag = tag;
    msg_info.size = size;
    msg_info.u.data = raddr;

    chpl_pvm_send(locale, TAGMASK+1, &msg_info, sizeof(_chpl_message_info));
    chpl_pvm_send(locale, tag, addr, size);

#if CHPL_DIST_DEBUG
    sprintf(debugMsg, "chpl_comm_put(loc=(%d, %p), rem=(%d, %p), size=%d, tag=%d) done", chpl_localeID, addr, locale, raddr, size, tag);
    PRINTF(debugMsg);
#endif
  }
}

void chpl_comm_get(void* addr, void* rref, int32_t size, int ln, chpl_string fn) {
  int32_t locale = ((chpl_wide_voidStar *)rref)->locale;
  void* raddr = ((chpl_wide_voidStar *)rref)->addr;
  if (chpl_localeID == locale) {
    if (size < 0) {
      chpl_internal_error("memmove error");
    }
#ifdef CHPL_COMM_HETEROGENEOUS
    memmove(raddr, addr, chpl_getFieldSize(size));
#else
    memmove(raddr, addr, size);
#endif
  } else {
    _chpl_message_info msg_info;
    int tag;

#if CHPL_DIST_DEBUG
    char debugMsg[DEBUG_MSG_LENGTH];
#endif

    if (chpl_comm_diagnostics && !chpl_comm_no_debug_private) {
      CHPL_MUTEX_LOCK(&chpl_comm_diagnostics_lock);
      chpl_comm_gets++;
      CHPL_MUTEX_UNLOCK(&chpl_comm_diagnostics_lock);
    }

    // Note: this isn't a PVM call, but we need this locked to make
    // sure PVM tags are unique.
    PVM_SAFE(tag = makeTag((int)pthread_self, chpl_localeID), "makeTag", "chpl_comm_get");

#if CHPL_DIST_DEBUG
    sprintf(debugMsg, "chpl_comm_get(loc=(%d, %p), rem=(%d, %p), size=%d, tag=%d)", chpl_localeID, addr, locale, raddr, size, tag);
    PRINTF(debugMsg);
#endif
    
    msg_info.msg_type = ChplCommGet;
    msg_info.replyTag = tag;
    msg_info.size = size;
    msg_info.u.data = (void *)raddr;

    chpl_pvm_send(locale, TAGMASK+1, &msg_info, sizeof(_chpl_message_info));
    chpl_pvm_recv(locale, tag, addr, size);

#if CHPL_DIST_DEBUG
    sprintf(debugMsg, "chpl_comm_get(loc=(%d, %p), rem=(%d, %p), size=%d, tag=%d) done", chpl_localeID, addr, locale, raddr, size, tag);
    PRINTF(debugMsg);
#endif
  }
}

void chpl_comm_fork(int locale, chpl_fn_int_t fid, void *arg, int arg_size) {
  if (chpl_localeID == locale) {
    (*chpl_ftable[fid])(arg);
  } else {
    _chpl_message_info msg_info;
    int tag;

#if CHPL_DIST_DEBUG
    char debugMsg[DEBUG_MSG_LENGTH];
#endif

    if (chpl_comm_diagnostics && !chpl_comm_no_debug_private) {
      CHPL_MUTEX_LOCK(&chpl_comm_diagnostics_lock);
      chpl_comm_forks++;
      CHPL_MUTEX_UNLOCK(&chpl_comm_diagnostics_lock);
    }

    // Note: this isn't a PVM call, but we need this locked to make
    // sure PVM tags are unique.
    PVM_SAFE(tag = makeTag((int)pthread_self, chpl_localeID), "makeTag", "chpl_comm_fork");

#if CHPL_DIST_DEBUG
    sprintf(debugMsg, "chpl_comm_fork(locale=%d, tag=%d)", locale, tag);
    PRINTF(debugMsg);
#endif
    
    msg_info.msg_type = ChplCommFork;
    msg_info.replyTag = tag;
    msg_info.size = arg_size;
    msg_info.u.fid = fid;
    
    chpl_pvm_send(locale, TAGMASK+1, &msg_info, sizeof(_chpl_message_info));
    chpl_pvm_send(locale, tag, arg, arg_size);
    chpl_pvm_recv(locale, tag, NULL, 0);
  }
}

void chpl_comm_fork_nb(int locale, chpl_fn_int_t fid, void *arg, int arg_size) {
  _chpl_message_info msg_info;
  int tag;

#if CHPL_DIST_DEBUG
  char debugMsg[DEBUG_MSG_LENGTH];
#endif

  int mallocsize;
#ifdef CHPL_COMM_HETEROGENEOUS
  if (arg_size < 0) {
    chpl_internal_error("memmove error");
  }
  mallocsize = chpl_getFieldSize(arg_size);
#else
  mallocsize = arg_size;
#endif

  if (chpl_localeID == locale) {
    void* argCopy = chpl_malloc(1, mallocsize, CHPL_RT_MD_REMOTE_NB_FORK_DATA, 0, 0);
    memmove(argCopy, arg, mallocsize);
    CHPL_BEGIN((chpl_fn_p)chpl_ftable[fid], argCopy, true, false, NULL);
  } else {
    if (chpl_comm_diagnostics && !chpl_comm_no_debug_private) {
      CHPL_MUTEX_LOCK(&chpl_comm_diagnostics_lock);
      chpl_comm_nb_forks++;
      CHPL_MUTEX_UNLOCK(&chpl_comm_diagnostics_lock);
    }

    // Note: this isn't a PVM call, but we need this locked to make
    // sure PVM tags are unique.
    PVM_SAFE(tag = makeTag((int)pthread_self, chpl_localeID), "makeTag", "chpl_comm_fork_nb");
#if CHPL_DIST_DEBUG
    sprintf(debugMsg, "chpl_comm_fork_nb(locale=%d, tag=%d)", locale, tag);
    PRINTF(debugMsg);
#endif
    
    msg_info.msg_type = ChplCommForkNB;
    msg_info.replyTag = tag;
    msg_info.size = arg_size;
    msg_info.u.fid = fid;
    
    chpl_pvm_send(locale, TAGMASK+1, &msg_info, sizeof(_chpl_message_info));
    chpl_pvm_send(locale, tag, arg, arg_size);
  }
}

// Macro to clean up broadcast_private calling for heterogeneous and
// homogeneous cases.
// index 0 = &chpl_verbose_comm
// index 1 = &chpl_comm_diagnostics
// index 2 = &chpl_verbose_mem
#ifdef CHPL_COMM_HETEROGENEOUS
#define COMM_SIZE(index) {                                                 \
  chpl_comm_broadcast_private(index, -CHPL_TYPE_int32_t);                  \
}
#else                                                                      
#define COMM_SIZE(index) {                                                 \
  chpl_comm_broadcast_private(index, sizeof(int));                         \
}
#endif

void chpl_startVerboseComm() {
  chpl_verbose_comm = 1;
  chpl_comm_no_debug_private = 1;
  COMM_SIZE(0);
  chpl_comm_no_debug_private = 0;
}

void chpl_stopVerboseComm() {
  chpl_verbose_comm = 0;
  chpl_comm_no_debug_private = 1;
  COMM_SIZE(0);
  chpl_comm_no_debug_private = 0;
}

void chpl_startVerboseCommHere() {
  chpl_verbose_comm = 1;
}

void chpl_stopVerboseCommHere() {
  chpl_verbose_comm = 0;
}

void chpl_startCommDiagnostics() {
  chpl_comm_diagnostics = 1;
  chpl_comm_no_debug_private = 1;
  COMM_SIZE(1);
  chpl_comm_no_debug_private = 0;
}

void chpl_stopCommDiagnostics() {
  chpl_comm_diagnostics = 0;
  chpl_comm_no_debug_private = 1;
  COMM_SIZE(1);
  chpl_comm_no_debug_private = 0;
}

void chpl_startCommDiagnosticsHere() {
  chpl_comm_diagnostics = 1;
}

void chpl_stopCommDiagnosticsHere() {
  chpl_comm_diagnostics = 0;
}

int32_t chpl_numCommGets(void) {
  return chpl_comm_gets;
}

int32_t chpl_numCommPuts(void) {
  return chpl_comm_puts;
}

int32_t chpl_numCommForks(void) {
  return chpl_comm_forks;
}

int32_t chpl_numCommNBForks(void) {
  return chpl_comm_nb_forks;
}


/* TODO: eventually make this a bit more clever, as with the
   make_message call on the vsprintf man page, in order to remove the
   statically-sized buffer and internal error if it overflows.  Let's
   get this working first, though */

#define PRINTF_BUFF_LEN 1024

#define CHPL_PVM_PRINTF_GUTS                                            \
  va_list args;                                                         \
  int retval;                                                           \
  char buffer[PRINTF_BUFF_LEN];                                         \
                                                                        \
  va_start(args, format);                                               \
  retval = vsnprintf(buffer, PRINTF_BUFF_LEN, format, args);            \
  if (retval > PRINTF_BUFF_LEN) {                                       \
    chpl_internal_error("PVM fprintf insufficient buffer space");       \
  }                                                                     \
  va_end(args)


/* Undefine these to avoid an infinite loop in the code below */
#undef printf
#undef fprintf

int chpl_pvm_fprintf(FILE* outfile, const char* format, ...) {
  int fdnum;
  CHPL_PVM_PRINTF_GUTS;

  fdnum = fileno(outfile);

  if ((parent >= 0) && ((fdnum == 0) || (fdnum == 1) || (fdnum == 2))) {
    commsig = 2;
    PVM_PACK_SAFE(pvm_initsend(PvmDataDefault), "pvm_initsend", "chpl_pvm_fprintf");
    PVM_NO_LOCK_SAFE(pvm_pkint(&commsig, 1, 1), "pvm_pkint", "chpl_pvm_fprintf");
    PVM_NO_LOCK_SAFE(pvm_pkint(&fdnum, 1, 1), "pvm_pkint", "chpl_pvm_fprintf");
    PVM_NO_LOCK_SAFE(pvm_pkstr(buffer), "pvm_pkstr", "chpl_pvm_fprintf");
    PVM_UNPACK_SAFE(pvm_send(parent, NOTIFYTAG), "pvm_pksend", "chpl_pvm_fprintf");
    commsig = 0;
  } else fprintf(outfile, "%s", buffer);
  return retval;
}


int chpl_pvm_printf(const char* format, ...) {
  CHPL_PVM_PRINTF_GUTS;

  if (parent >= 0) {
    commsig = 3;

    PVM_PACK_SAFE(pvm_initsend(PvmDataDefault), "pvm_initsend", "chpl_pvm_printf");
    PVM_NO_LOCK_SAFE(pvm_pkint(&commsig, 1, 1), "pvm_pkint", "chpl_pvm_printf");
    PVM_NO_LOCK_SAFE(pvm_pkstr(buffer), "pvm_pkstr", "chpl_pvm_printf");
    PVM_UNPACK_SAFE(pvm_send(parent, NOTIFYTAG), "pvm_pksend", "chpl_pvm_printf");
    commsig = 0;
  } else printf("%s", buffer);
  return retval;
}


int chpl_pvm_vfprintf(FILE* stream, const char* format, va_list ap) {
  int fdnum;
  int retval;
  char buffer[PRINTF_BUFF_LEN];

  retval = vsnprintf(buffer, PRINTF_BUFF_LEN, format, ap);
  if (retval > PRINTF_BUFF_LEN) {
    chpl_internal_error("PVM fprintf insufficient buffer space");
  }
  
  fdnum = fileno(stream);

  if ((parent >= 0) && ((fdnum == 0) || (fdnum == 1) || (fdnum == 2))) {
    commsig = 2;
    PVM_PACK_SAFE(pvm_initsend(PvmDataDefault), "pvm_initsend", "chpl_pvm_fprintf");
    PVM_NO_LOCK_SAFE(pvm_pkint(&commsig, 1, 1), "pvm_pkint", "chpl_pvm_fprintf");
    PVM_NO_LOCK_SAFE(pvm_pkint(&fdnum, 1, 1), "pvm_pkint", "chpl_pvm_fprintf");
    PVM_NO_LOCK_SAFE(pvm_pkstr(buffer), "pvm_pkstr", "chpl_pvm_fprintf");
    PVM_UNPACK_SAFE(pvm_send(parent, NOTIFYTAG), "pvm_pksend", "chpl_pvm_fprintf");
    commsig = 0;
  } else fprintf(stream, "%s", buffer);
  return retval;
}


/* NOTHING SHOULD GO BELOW THESE FUNCTIONS (PARTICULARLY, NOTHING THAT
   USES PRINTF) */
