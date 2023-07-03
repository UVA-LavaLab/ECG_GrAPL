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
// ***************                  ECG Generate  (P-OPT based)                  **************
// ********************************************************************************************
uint32_t *makeOffsetMatrixProcess(struct GraphCSR *graph, struct Arguments *arguments)
{
    uint32_t j;
    uint32_t v;
    uint32_t u;
    uint32_t cl;
    uint32_t degree;
    uint32_t edge_idx;
    struct Timer *timer      = (struct Timer *) malloc(sizeof(struct Timer));
    uint32_t vertexPerCacheLine    = 64 / sizeof(uint32_t);
    uint32_t numCacheLines = (graph->num_vertices + vertexPerCacheLine - 1) / vertexPerCacheLine;
    uint32_t numEpochs     = 256;
    uint32_t epochSize     = (graph->num_vertices + numEpochs - 1) / numEpochs;
    uint32_t chunkSize     = 64 / vertexPerCacheLine;
    if (chunkSize == 0)
        chunkSize = 1;

    uint32_t *last_references = (uint32_t *) my_malloc((numCacheLines * numEpochs) * sizeof(uint32_t));
    uint8_t  *offset_matrix   = (uint8_t *) my_malloc((numCacheLines * numEpochs) * sizeof(uint8_t));

    struct Vertex *vertices = NULL;
    uint32_t *sorted_edges_array = NULL;

#if DIRECTED
    vertices = graph->inverse_vertices;
    sorted_edges_array = graph->inverse_sorted_edges_array->edges_array_dest;
#else
    vertices = graph->vertices;
    sorted_edges_array = graph->sorted_edges_array->edges_array_dest;
#endif

    /* Step I: Collect quantized edges & Compact vertices into "super vertices" */
    printf(" -----------------------------------------------------\n");
    printf("| %-51s | \n", "P-OPT Quantize Graph START");
    printf(" -----------------------------------------------------\n");
    Start(timer);
    #pragma omp parallel for schedule(dynamic, chunkSize)
    for (cl = 0; cl < numCacheLines; ++cl)
    {

        uint32_t start_vertex = cl * vertexPerCacheLine;
        uint32_t end_vertex   = (cl + 1) * vertexPerCacheLine;
        if (cl == numCacheLines - 1)
            end_vertex = graph->num_vertices;

        for (v = start_vertex; v < end_vertex; ++v)
        {
            degree = vertices->out_degree[v];
            edge_idx = vertices->edges_idx[v];
            for(j = edge_idx ; j < (edge_idx + degree) ; j++)
            {
                u = EXTRACT_VALUE(sorted_edges_array[j]);
                uint32_t u_epoch = u / epochSize;
                last_references[(cl * numEpochs) + u_epoch] = maxTwoIntegers(u, last_references[(cl * numEpochs) + u_epoch]);
            }
        }

    }
    Stop(timer);

    printf(" -----------------------------------------------------\n");
    printf("| %-51s | \n", "P-OPT Quantize Graph Complete");
    printf(" -----------------------------------------------------\n");
    printf("| %-51f | \n", Seconds(timer));
    printf(" -----------------------------------------------------\n");

    /* Step II: Converting adjacency matrix into offsets */
    uint8_t maxReref = 127; //because MSB is reserved for identifying between reref val (1) & switch point (0)
    uint32_t subEpochSz = (epochSize + 127) / 128; //Using remaining 7 bits to identify intra-epoch information
    uint8_t mask    = 1;
    uint8_t orMask  = mask << 7;
    uint8_t andMask = ~(orMask);

    uint8_t *compressedOffsets = (uint8_t *) my_malloc((numCacheLines * numEpochs) * sizeof(uint8_t));

    printf(" -----------------------------------------------------\n");
    printf("| %-51s | \n", "P-OPT Converting adjacency matrix into offsets START");
    printf(" -----------------------------------------------------\n");
    Start(timer);
    #pragma omp parallel for schedule (static)
    for (uint32_t cl = 0; cl < numCacheLines; ++cl)
    {
        {
            // first set values for the last epoch
            uint32_t epoch = numEpochs - 1;
            if (last_references[(cl * numEpochs) + epoch] != -1)
            {
                compressedOffsets[(cl * numEpochs) + epoch] = maxReref;
                compressedOffsets[(cl * numEpochs) + epoch] &= andMask;
            }
            else
            {
                compressedOffsets[(cl * numEpochs) + epoch] = maxReref;
                compressedOffsets[(cl * numEpochs) + epoch] |= orMask;
            }
        }

        //Now back track and set values for all epochs
        uint32_t epoch;
        for (epoch = numEpochs - 2; epoch >= 0; --epoch)
        {
            if (last_references[(cl * numEpochs) + epoch] != -1)
            {
                // There was a ref this epoch - store the quantized val of the last_references
                uint32_t subEpochDist = last_references[(cl * numEpochs) + epoch] - (epoch * epochSize);
                assert(subEpochDist >= 0);
                uint32_t lastRefQ = (subEpochDist / subEpochSz);
                assert(lastRefQ <= maxReref);
                compressedOffsets[(cl * numEpochs) + epoch] = static_cast<uint8_t>(lastRefQ);
                compressedOffsets[(cl * numEpochs) + epoch] &= andMask;
            }
            else
            {
                if ((compressedOffsets[(cl * numEpochs) + epoch + 1] & orMask) != 0)
                {
                    //No access next epoch as well - add inter-epoch distance
                    uint8_t nextRef = compressedOffsets[(cl * numEpochs) + epoch + 1] & andMask;
                    if (nextRef == maxReref)
                        compressedOffsets[(cl * numEpochs) + epoch] = maxReref;
                    else
                        compressedOffsets[(cl * numEpochs) + epoch] = nextRef + 1;
                }
                else
                {
                    //There is an access next epoch - so inter-epoch distance is set to next epoch
                    compressedOffsets[(cl * numEpochs) + epoch] = 1;
                }
                compressedOffsets[(cl * numEpochs) + epoch] |= orMask;
            }
        }
    }
    Stop(timer);
    printf(" -----------------------------------------------------\n");
    printf("| %-51s | \n", "P-OPT Converting adjacency matrix into offsets Complete");
    printf(" -----------------------------------------------------\n");
    printf("| %-51f | \n", Seconds(timer));
    printf(" -----------------------------------------------------\n");


    free(timer);
    free(last_references);
    free(compressedOffsets);
    return offset_matrix;
}
uint32_t *makeRerefrenceMaskProcess(struct GraphCSR *graph,  struct Arguments *arguments)
{


    return NULL;
}
uint32_t *makePrefetchMaskProcess(struct GraphCSR *graph, struct Arguments *arguments)
{


    return NULL;
}


