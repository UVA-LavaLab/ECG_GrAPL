#ifndef ECGRUN_H
#define ECGRUN_H

#include <stdint.h>
#include "edgeList.h"
#include "graphCSR.h"

#include "cache.h"


// ********************************************************************************************
// ***************                  ECG Generate                                 **************
// ********************************************************************************************
uint8_t  * makeOffsetMatrixProcess(struct GraphCSR *graph, struct Arguments *arguments);
uint32_t * makeRerefrenceMaskProcess(struct GraphCSR *graph,  struct Arguments *arguments);
uint32_t * makePrefetchMaskProcess(struct GraphCSR *graph, struct Arguments *arguments);

#endif