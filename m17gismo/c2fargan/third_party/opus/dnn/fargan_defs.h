/* fargan_defs.h - constants FARGAN takes from parts of Opus that are not copied here.
   Values match Opus (lpcnet.h, freq.h, pitchdnn.h). Not part of upstream Opus. */
#ifndef FARGAN_DEFS_H
#define FARGAN_DEFS_H

/* fargan.c got these through the LPCNet headers that are left out */
#include <math.h>
#include "arch.h"

#define NB_FEATURES 20
#define NB_TOTAL_FEATURES 36
#define NB_BANDS 18
#define PITCH_MAX_PERIOD 256
#define LPCNET_FRAME_SIZE 160

/* nnet.c sizes its scratch buffers for every Opus model. Only FARGAN is built here. */
#define PLC_MAX_RNN_UNITS 0
#define DRED_MAX_RNN_NEURONS 0
#define DRED_MAX_CONV_INPUTS 0

#endif
