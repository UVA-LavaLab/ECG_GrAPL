// -----------------------------------------------------------------------------
//
//      "00_AccelGraph"
//
// -----------------------------------------------------------------------------
// Copyright (c) 2014-2019 All rights reserved
// -----------------------------------------------------------------------------
// Author : Abdullah Mughrabi
// Email  : atmughra@ncsu.edu||atmughrabi@gmail.com
// File   : reorder.c
// Create : 2019-06-21 17:15:17
// Revise : 2019-09-28 15:35:52
// Editor : Abdullah Mughrabi
// -----------------------------------------------------------------------------
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <math.h>
#include <omp.h>
#include <stdint.h>

#include "timer.h"
#include "myMalloc.h"
#include "graphConfig.h"
#include "edgeList.h"
#include "sortRun.h"
#include "vc_vector.h"

#include "graphCSR.h"
#include "reorder.h"
#include "ecgRun.h"



// ********************************************************************************************
// ***************                  ECG Generate                                 **************
// ********************************************************************************************
uint32_t *makeOffsetMatrixProcess(struct GraphCSR *graph, struct Arguments *arguments)
{

    struct Timer *timer      = (struct Timer *) malloc(sizeof(struct Timer));
    uint32_t vertexPerCacheLine    = 64 / sizeof(uint32_t);
    uint32_t numCacheLines = (graph->num_vertices + vertexPerCacheLine - 1) / vertexPerCacheLine;
    uint32_t numEpochs     = 256;
    uint32_t chunkSize     = 64 / vertexPerCacheLine;
    if (chunkSize == 0)
        chunkSize = 1;

    uint32_t *last_references = (uint32_t *) my_malloc((numCacheLines * numEpochs) * sizeof(uint32_t));

    #pragma omp parallel for schedule(dynamic, chunkSize)
    for (uint32_t cl = 0; cl < numCacheLines; ++cl)
    {
    

    }

    free(last_references);
    return NULL;
}
uint32_t *makeRerefrenceMaskProcess(struct GraphCSR *graph,  struct Arguments *arguments)
{


    return NULL;
}
uint32_t *makePrefetchMaskProcess(struct GraphCSR *graph, struct Arguments *arguments)
{


    return NULL;
}


