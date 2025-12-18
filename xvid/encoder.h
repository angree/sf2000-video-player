/*
 * Encoder stub for SF2000 decoder-only build
 */
#ifndef _ENCODER_H_
#define _ENCODER_H_

/* Empty encoder structure - we only decode */
typedef struct {
    int dummy;
} Encoder;

/* Stub encoder functions */
static inline int enc_create(void *create) { return -1; }
static inline int enc_destroy(void *handle) { return -1; }
static inline int enc_encode(void *handle, void *frame, void *stats) { return -1; }

#endif /* _ENCODER_H_ */
