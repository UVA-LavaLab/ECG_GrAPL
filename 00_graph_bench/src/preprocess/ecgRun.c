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
void makeOffsetMatrixProcess(struct GraphCSR *graph, struct Arguments *arguments)
{
    uint32_t i;
    uint32_t j;
    uint32_t v;
    uint32_t u;
    uint32_t cl;
    uint32_t degree;
    uint32_t edge_idx;
    struct Timer *timer      = (struct Timer *) malloc(sizeof(struct Timer));
    uint32_t vertexPerCacheLine    = 64 / sizeof(uint32_t);
    uint32_t numCacheLines = (graph->num_vertices + vertexPerCacheLine - 1) / vertexPerCacheLine;
    uint32_t numEpochs     = (uint32_t)(1 << POPT_CACHE_BITS);
    uint32_t epochSize     = (graph->num_vertices + numEpochs - 1) / numEpochs;
    uint32_t chunkSize     = 64 / vertexPerCacheLine;
    if (chunkSize == 0)
        chunkSize = 1;

    int *last_references = (int *) my_malloc((numCacheLines * numEpochs) * sizeof(int));
    graph->offset_matrix   = (uint32_t *) my_malloc((numCacheLines * numEpochs) * sizeof(uint32_t));

    #pragma omp parallel for
    for (i = 0; i < (numCacheLines * numEpochs); ++i)
    {
        last_references[i] = -1;
    }

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
                last_references[(cl * numEpochs) + u_epoch] = maxTwoIntegersSigned(u, last_references[(cl * numEpochs) + u_epoch]);
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

    #pragma omp parallel for
    for (i = 0; i < (numCacheLines * numEpochs); ++i)
    {
        compressedOffsets[i] = 0;
    }

    printf(" -----------------------------------------------------\n");
    printf("| %-51s | \n", "P-OPT Convert adjacency matrix -> offsets START");
    printf(" -----------------------------------------------------\n");
    Start(timer);
    #pragma omp parallel for schedule (static)
    for (cl = 0; cl < numCacheLines; ++cl)
    {
        {
            // first set values for the last epoch

            int epoch = numEpochs - 1;
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
        int epoch;
        for (epoch = numEpochs - 2; epoch >= 0; --epoch)
        {
            // printf("last_references cl %u numEpochs %u epoch %u index %u value %d \n",cl, numEpochs, epoch, ((cl * numEpochs) + epoch), last_references[(cl * numEpochs) + epoch] );
            if (last_references[(cl * numEpochs) + epoch] != -1)
            {
                // There was a ref this epoch - store the quantized val of the last_references
                uint32_t subEpochDist = last_references[(cl * numEpochs) + epoch] - (epoch * epochSize);
                uint32_t lastRefQ = (subEpochDist / subEpochSz);
                compressedOffsets[(cl * numEpochs) + epoch] = (uint8_t)lastRefQ;
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
    printf("| %-51s | \n", "P-OPT Convert adjacency matrix -> offsets Complete");
    printf(" -----------------------------------------------------\n");
    printf("| %-51f | \n", Seconds(timer));
    printf(" -----------------------------------------------------\n");


    /* Step III: Transpose edgePresent*/
    printf(" -----------------------------------------------------\n");
    printf("| %-51s | \n", "P-OPT Transpose offset matrix START");
    printf(" -----------------------------------------------------\n");
    Start(timer);
    #pragma omp parallel for schedule (static)
    for (cl = 0; cl < numCacheLines; ++cl)
    {
        int epoch;
        for (epoch = 0; epoch < numEpochs; ++epoch)
        {
            graph->offset_matrix[(epoch * numCacheLines) + cl] = compressedOffsets[(cl * numEpochs) + epoch];
        }
    }
    Stop(timer);
    printf(" -----------------------------------------------------\n");
    printf("| %-51s | \n", "P-OPT Convert adjacency matrix -> offsets Complete");
    printf(" -----------------------------------------------------\n");
    printf("| %-51f | \n", Seconds(timer));
    printf(" -----------------------------------------------------\n");

    free(timer);
    free(last_references);
    free(compressedOffsets);

    // printOffsetMatrixProcessParameterized(graph,arguments);
}


void makeOffsetMatrixProcessParameterized(struct GraphCSR *graph, struct Arguments *arguments)
{
    uint32_t i;
    uint32_t j;
    uint32_t v;
    uint32_t u;
    uint32_t cl;
    uint32_t degree;
    uint32_t edge_idx;
    struct Timer *timer      = (struct Timer *) malloc(sizeof(struct Timer));
    uint32_t vertexPerCacheLine    = CACHELINE_BYTES / sizeof(uint32_t);
    uint32_t numCacheLines = (graph->num_vertices + vertexPerCacheLine - 1) / vertexPerCacheLine;
    uint32_t numEpochs     = (uint32_t)(1 << arguments->popt_bits);
    uint32_t epochSize     = (graph->num_vertices + numEpochs - 1) / numEpochs;
    uint32_t chunkSize     = CACHELINE_BYTES / vertexPerCacheLine;
    if (chunkSize == 0)
        chunkSize = 1;

    int *last_references = (int *) my_malloc((numCacheLines * numEpochs) * sizeof(int));
    graph->offset_matrix   = (uint32_t *) my_malloc((numCacheLines * numEpochs) * sizeof(uint32_t));

    #pragma omp parallel for
    for (i = 0; i < (numCacheLines * numEpochs); ++i)
    {
        last_references[i] = -1;
    }

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
                last_references[(cl * numEpochs) + u_epoch] = maxTwoIntegersSigned(u, last_references[(cl * numEpochs) + u_epoch]);
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
    uint32_t maxReref = (uint32_t)((1 << (arguments->popt_bits -1))-1); //because MSB is reserved for identifying between reref val (1) & switch point (0)
    uint32_t subEpochSz = (epochSize + maxReref) / (maxReref +1); //Using remaining 7 bits to identify intra-epoch information
    uint32_t mask    = 1;
    uint32_t orMask  = mask << (arguments->popt_bits -1);
    uint32_t andMask = ~(orMask);

    uint32_t *compressedOffsets = (uint32_t *) my_malloc((numCacheLines * numEpochs) * sizeof(uint32_t));

    #pragma omp parallel for
    for (i = 0; i < (numCacheLines * numEpochs); ++i)
    {
        compressedOffsets[i] = 0;
    }

    printf(" -----------------------------------------------------\n");
    printf("| %-51s | \n", "P-OPT Convert adjacency matrix -> offsets START");
    printf(" -----------------------------------------------------\n");
    Start(timer);
    #pragma omp parallel for schedule (static)
    for (cl = 0; cl < numCacheLines; ++cl)
    {
        {
            // first set values for the last epoch

            int epoch = numEpochs - 1;
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
        int epoch;
        for (epoch = numEpochs - 2; epoch >= 0; --epoch)
        {
            // printf("last_references cl %u numEpochs %u epoch %u index %u value %d \n",cl, numEpochs, epoch, ((cl * numEpochs) + epoch), last_references[(cl * numEpochs) + epoch] );
            if (last_references[(cl * numEpochs) + epoch] != -1)
            {
                // There was a ref this epoch - store the quantized val of the last_references
                uint32_t subEpochDist = last_references[(cl * numEpochs) + epoch] - (epoch * epochSize);
                uint32_t lastRefQ = (subEpochDist / subEpochSz);
                compressedOffsets[(cl * numEpochs) + epoch] = (uint32_t)lastRefQ;
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
    printf("| %-51s | \n", "P-OPT Convert adjacency matrix -> offsets Complete");
    printf(" -----------------------------------------------------\n");
    printf("| %-51f | \n", Seconds(timer));
    printf(" -----------------------------------------------------\n");


    /* Step III: Transpose edgePresent*/
    printf(" -----------------------------------------------------\n");
    printf("| %-51s | \n", "P-OPT Transpose offset matrix START");
    printf(" -----------------------------------------------------\n");
    Start(timer);
    #pragma omp parallel for schedule (static)
    for (cl = 0; cl < numCacheLines; ++cl)
    {
        int epoch;
        for (epoch = 0; epoch < numEpochs; ++epoch)
        {
            graph->offset_matrix[(epoch * numCacheLines) + cl] = compressedOffsets[(cl * numEpochs) + epoch];
        }
    }
    Stop(timer);
    printf(" -----------------------------------------------------\n");
    printf("| %-51s | \n", "P-OPT Convert adjacency matrix -> offsets Complete");
    printf(" -----------------------------------------------------\n");
    printf("| %-51f | \n", Seconds(timer));
    printf(" -----------------------------------------------------\n");

    free(timer);
    free(last_references);
    free(compressedOffsets);

    // printOffsetMatrixProcessParameterized(graph,arguments);
}

void printOffsetMatrixProcessParameterized(struct GraphCSR *graph, struct Arguments *arguments){

    uint32_t cl;
    uint32_t vertexPerCacheLine    = CACHELINE_BYTES / sizeof(uint32_t);
    uint32_t numCacheLines = (graph->num_vertices + vertexPerCacheLine - 1) / vertexPerCacheLine;
    uint32_t numEpochs     = (uint32_t)(1 << arguments->popt_bits);


    printf(" -----------------------------------------------------\n");
    printf("| %-15s | %-15s | %-15s | \n", "CLID", "Epoch", "offset_matrix");
    printf(" -----------------------------------------------------\n");
    for (cl = 0; cl < numCacheLines; ++cl)
    {
        uint32_t epoch;
        for (epoch = 0; epoch < numEpochs; ++epoch)
        {
            printf("| %-15u , %-15u , %-15u |\n", cl, epoch, graph->offset_matrix[(epoch * numCacheLines) + cl]);
        }
    }
    printf(" -----------------------------------------------------\n");
    printf("| %-15s | \n", "numCacheLines");
    printf(" -----------------------------------------------------\n");
    printf("| %-15u |\n", numCacheLines);
    printf(" -----------------------------------------------------\n");


}

void printOffsetMatrixPreftechParameterized(struct GraphCSR *graph, struct Arguments *arguments){

    uint32_t i;
    struct Vertex *vertices = NULL;

#if DIRECTED
    vertices = graph->inverse_vertices;
#else
    vertices = graph->vertices;
#endif
    printf(" -----------------------------------------------------\n");
    printf("| %-15s | %-15s | %-15s | \n", "v", "degree", "prefetch_matrix");
    printf(" -----------------------------------------------------\n");
    for (i = 0; i < graph->num_vertices; ++i)
    {
        printf("| %-15u , %-15u , %-15u |\n", i, vertices->out_degree[graph->prefetch_matrix[i]], graph->prefetch_matrix[i]);
    }
    printf(" -----------------------------------------------------\n");


}

void makePrefetchMaskProcess(struct GraphCSR *graph, struct Arguments *arguments)
{

    uint32_t i;
    uint32_t j;
    uint32_t v;
    uint32_t u;
    uint32_t degree;
    uint32_t max_degree;
    uint32_t max_degree_id;
    uint32_t edge_idx;
    struct Vertex *vertices = NULL;
    uint32_t *sorted_edges_array = NULL;

    graph->prefetch_matrix   = (uint32_t *) my_malloc((graph->num_vertices+1) * sizeof(uint32_t));


    #pragma omp parallel for
    for (i = 0; i < graph->num_vertices; ++i)
    {
        graph->prefetch_matrix[i] = 0;
    }

#if DIRECTED
    vertices = graph->inverse_vertices;
    sorted_edges_array = graph->inverse_sorted_edges_array->edges_array_dest;
#else
    vertices = graph->vertices;
    sorted_edges_array = graph->sorted_edges_array->edges_array_dest;
#endif

    #pragma omp parallel for private(v,j,u,degree,max_degree,max_degree_id,edge_idx) schedule(dynamic, 1024) num_threads(arguments->ker_numThreads)
    for(v = 0; v < graph->num_vertices; v++)
    {
        degree = vertices->out_degree[v];
        edge_idx = vertices->edges_idx[v];
        max_degree = 0;
        max_degree_id = 0;
        for(j = edge_idx ; j < (edge_idx + degree) ; j++)
        {
            u = EXTRACT_VALUE(sorted_edges_array[j]);
            if(vertices->out_degree[u] > max_degree){
                max_degree = vertices->out_degree[u];
                max_degree_id = u;
            }
        }
        graph->prefetch_matrix[v] = max_degree_id;
    }
    // printOffsetMatrixPreftechParameterized(graph,arguments);
}


