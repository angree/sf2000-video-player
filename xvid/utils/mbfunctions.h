/*
 * Macroblock functions header for SF2000
 * Minimal declarations needed for decoder
 */
#ifndef _MBFUNCTIONS_H_
#define _MBFUNCTIONS_H_

#include "../portab.h"

/* Interlacing field test function type */
typedef uint32_t (MBFIELDFUNC)(int16_t * const data);
typedef MBFIELDFUNC *MBFieldTestPtr;

/* Global function pointer */
extern MBFieldTestPtr MBFieldTest;

/* C implementation */
uint32_t MBFieldTest_c(int16_t * const data);

#endif /* _MBFUNCTIONS_H_ */
