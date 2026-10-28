/* Modern > Save by itself every 10 minutes (autosave.c). */
#pragma once
#include <stdint.h>

extern int g_autosave;             /* on */
extern int g_autosave_done;        /* it just saved: the frontend says so and clears this */
