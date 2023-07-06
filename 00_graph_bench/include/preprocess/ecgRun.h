#ifndef ECGRUN_H
#define ECGRUN_H

#include <stdint.h>
#include "edgeList.h"
#include "graphCSR.h"

#include "cache.h"


// ********************************************************************************************
// ***************                  ECG Generate                                 **************
// ********************************************************************************************
void makeOffsetMatrixProcess(struct GraphCSR *graph, struct Arguments *arguments);
void makeOffsetMatrixProcessParameterized(struct GraphCSR *graph, struct Arguments *arguments);
void printOffsetMatrixProcessParameterized(struct GraphCSR *graph, struct Arguments *arguments);
uint32_t * makeRerefrenceMaskProcess(struct GraphCSR *graph,  struct Arguments *arguments);
uint32_t * makePrefetchMaskProcess(struct GraphCSR *graph, struct Arguments *arguments);

#endif