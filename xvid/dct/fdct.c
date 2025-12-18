/*
 * Forward DCT stub for SF2000 decoder-only build
 * FDCT is only used for encoding - this is a no-op
 */
#include "fdct.h"

/* Global function pointer */
fdctFuncPtr fdct = fdct_int32;

/* Stub implementation - decoder doesn't need FDCT */
void fdct_int32(short * const block)
{
    /* Empty - only encoding uses forward DCT */
    (void)block;
}
