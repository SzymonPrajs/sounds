#ifndef SOUNDS_DEFER_H
#define SOUNDS_DEFER_H

#include <stddefer.h>

#ifndef __STDC_DEFER_TS25755__
#error "sounds requires Clang 22+ with -std=c2y -fdefer-ts"
#endif

#endif
