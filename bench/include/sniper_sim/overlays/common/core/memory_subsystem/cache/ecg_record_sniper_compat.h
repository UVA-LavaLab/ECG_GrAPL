#ifndef GRAPHBREW_SNIPER_ECG_RECORD_COMPAT_H
#define GRAPHBREW_SNIPER_ECG_RECORD_COMPAT_H

#ifdef INVALID_ADDRESS
#pragma push_macro("INVALID_ADDRESS")
#undef INVALID_ADDRESS
#define GRAPHBREW_RESTORE_INVALID_ADDRESS
#endif

#include "ecg_record_native.h"
#include "ecg_record_runtime.h"

#ifdef GRAPHBREW_RESTORE_INVALID_ADDRESS
#pragma pop_macro("INVALID_ADDRESS")
#undef GRAPHBREW_RESTORE_INVALID_ADDRESS
#endif

#endif
