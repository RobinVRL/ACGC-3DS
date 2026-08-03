#ifndef PC_SAVE_PATHS_H
#define PC_SAVE_PATHS_H

/* Keep the memory-card emulation and the platform bootstrap pointed at the
 * same storage root. Relative paths work for the desktop port, but a 3DSX's
 * working directory is launcher-dependent and must not be used for saves. */
#ifdef TARGET_3DS
#include "acgc_3ds_paths.h"
#define PC_SAVE_DIR   ACGC_3DS_SAVE_DIR
#else
#define PC_SAVE_DIR   "save"
#endif

#define PC_CARD_A_DIR PC_SAVE_DIR "/card_a"
#define PC_CARD_B_DIR PC_SAVE_DIR "/card_b"

#endif
