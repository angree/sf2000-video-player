/*
 * Forward DCT stub for SF2000 decoder-only build
 * FDCT is only used for encoding
 */
#ifndef _FDCT_H_
#define _FDCT_H_

#include "../portab.h"

/* Function pointer type */
typedef void (fdctFunc)(short * const block);
typedef fdctFunc *fdctFuncPtr;

/* Global function pointer (needed by xvid.c init) */
extern fdctFuncPtr fdct;

/* C implementation declaration */
void fdct_int32(short * const block);

#endif /* _FDCT_H_ */
