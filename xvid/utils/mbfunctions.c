/*
 * Macroblock functions for SF2000
 * MBFieldTest is used for interlaced video detection
 */
#include "mbfunctions.h"

/* Global function pointer */
MBFieldTestPtr MBFieldTest = MBFieldTest_c;

/*
 * Field test for interlacing detection
 * Returns non-zero if field mode should be used
 *
 * For SF2000 we assume progressive (non-interlaced) video
 * so we can use a simple implementation
 */
uint32_t MBFieldTest_c(int16_t * const data)
{
    /*
     * This tests whether a macroblock should be coded as field or frame.
     * For our decoder-only use, we just need to provide the decision
     * based on the data pattern.
     *
     * Simple heuristic: check variance between adjacent rows
     * If field motion is detected, return 1 (use field mode)
     */
    int frame_var = 0;
    int field_var = 0;
    int i, j;

    /* Compare adjacent rows (frame) vs alternate rows (field) */
    for (i = 0; i < 8; i++) {
        for (j = 0; j < 8; j++) {
            int idx = i * 8 + j;
            if (i < 7) {
                int diff_frame = data[idx] - data[idx + 8];
                frame_var += diff_frame * diff_frame;
            }
            if (i < 6) {
                int diff_field = data[idx] - data[idx + 16];
                field_var += diff_field * diff_field;
            }
        }
    }

    /* If field variance is significantly lower, use field mode */
    return (field_var < frame_var * 3 / 4) ? 1 : 0;
}
