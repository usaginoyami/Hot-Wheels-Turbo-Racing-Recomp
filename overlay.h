#pragma once
#include "runtime.h"
typedef struct { const char* name; uint32_t base, size; uint64_t hash; const FuncEntry* tab; int n; } Overlay;
extern Overlay overlays[]; extern const int overlays_n;
void overlay_init(void); void overlay_invalidate(void);
