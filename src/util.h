/* SPDX-License-Identifier: MIT */
#ifndef AUDIO_PROCESS_UTIL_H
#define AUDIO_PROCESS_UTIL_H

#include <stdint.h>

static inline int16_t clamp_i16(float v)
{
	if (v > 32767.0f)
		return 32767;
	if (v < -32768.0f)
		return -32768;
	return (int16_t)(int32_t)v;
}

static inline float clamp_f(float v, float lo, float hi)
{
	v = (v < lo) ? lo : v;
	return (v > hi) ? hi : v;
}

#endif
