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
    uint32_t chunkSize     = 64 / vertexPerCacheLine;
    if (chunkSize == 0)
        chunkSize = 1;

    uint32_t *last_references = (uint32_t *) my_malloc((numCacheLines * numEpochs) * sizeof(uint32_t));
    uint32_t *offset_matrix   = (uint32_t *) my_malloc((numCacheLines * numEpochs) * sizeof(uint32_t));

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
    Start(timer);
    #pragma omp parallel for schedule(dynamic, chunkSize)
    for (cl = 0; cl < numCacheLines; ++cl)
    {

        uint32_t start_vertex = cl * vertexPerCacheLine;
        uint32_t end_vertex   = (cl + 1) * vertexPerCacheLine;
        if (cl == numCacheLines - 1)
            end_vertex = graph->num_vertices;

        for (uint32_t v = start_vertex; v < end_vertex; ++v)
        {
            degree = vertices->out_degree[v];
            edge_idx = vertices->edges_idx[v];
            for(j = edge_idx ; j < (edge_idx + degree) ; j++)
            {
                u = EXTRACT_VALUE(sorted_edges_array[j]);
            }
        }

    }
    Stop(timer);

    printf(" -----------------------------------------------------\n");
    printf("| %-51s | \n", "Quantize Graph Complete");
    printf(" -----------------------------------------------------\n");
    printf("| %-51f | \n", Seconds(timer));
    printf(" -----------------------------------------------------\n");

    /* Step II: Converting adjacency matrix into offsets */


    free(timer);
    free(last_references);
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


