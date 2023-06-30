#ifndef GRAPHCONFIG_H
#define GRAPHCONFIG_H

#include <stdio.h>
#include <stdint.h>
#include "mt19937.h"

#define WEIGHTED 0
#define DIRECTED 1


#define VERTEX_T_SIZE 4
// #define VERTEX_T_SIZE 8

#define VERTEX_VALUE_MASK_U64 0xFFFFFFFF00000000
#define VERTEX_CACHE_MASK_U64 0x00000000FFFFFFFF

#define VERTEX_VALUE_HOT_U64       0xC000000000000000
#define VERTEX_CACHE_WARM_U64      0x8000000000000000
#define VERTEX_VALUE_LUKEWARM_U64  0x4000000000000000
#define VERTEX_CACHE_COLD_U64      0x0000000000000000

#define VERTEX_VALUE_MASK_U32 0xC0000000
#define VERTEX_CACHE_MASK_U32 0x3FFFFFFF

#define VERTEX_VALUE_HOT_U32       0xC0000000
#define VERTEX_CACHE_WARM_U32      0x80000000
#define VERTEX_VALUE_LUKEWARM_U32  0x40000000
#define VERTEX_CACHE_COLD_U32      0x00000000

#if (VERTEX_T_SIZE == 8)

typedef uint64_t vertex_t;
typedef uint64_t edge_t;

#define VERTEX_VALUE_MASK VERTEX_VALUE_MASK_U64
#define VERTEX_CACHE_MASK VERTEX_CACHE_MASK_U64

#define VERTEX_VALUE_HOT       VERTEX_VALUE_HOT_U64
#define VERTEX_CACHE_WARM      VERTEX_CACHE_WARM_U64
#define VERTEX_VALUE_LUKEWARM  VERTEX_VALUE_LUKEWARM_U64
#define VERTEX_CACHE_COLD      VERTEX_CACHE_COLD_U64

#else

typedef uint32_t vertex_t;
typedef uint64_t edge_t;

#define VERTEX_VALUE_MASK VERTEX_VALUE_MASK_U32
#define VERTEX_CACHE_MASK VERTEX_CACHE_MASK_U32

#define VERTEX_VALUE_HOT       VERTEX_VALUE_HOT_U32
#define VERTEX_CACHE_WARM      VERTEX_CACHE_WARM_U32
#define VERTEX_VALUE_LUKEWARM  VERTEX_VALUE_LUKEWARM_U32
#define VERTEX_CACHE_COLD      VERTEX_CACHE_COLD_U32

#endif

/* Used by main to communicate with parse_opt. */
struct Arguments
{
    int wflag;
    int xflag;
    int sflag;
    int Sflag;
    int dflag;
    vertex_t binSize;
    vertex_t verbosity;
    vertex_t iterations;
    vertex_t trials;
    double epsilon;
    int source;
    vertex_t algorithm;
    vertex_t datastructure;
    vertex_t pushpull;
    vertex_t sort;
    vertex_t lmode; // reorder mode
    vertex_t lmode_l2; // reorder mode second layer
    vertex_t lmode_l3; // reorder mode third layer
    vertex_t mmode; // mask mode
    vertex_t symmetric;
    vertex_t weighted;
    vertex_t delta;
    int pre_numThreads;
    int algo_numThreads;
    int ker_numThreads;
    char *fnameb;
    char *fnamel;
    vertex_t fnameb_format;
    vertex_t convert_format;
    mt19937state mt19937var;

#ifdef CACHE_HARNESS_META
    vertex_t l1_size;
    vertex_t l1_assoc;
    vertex_t blocksize;
    vertex_t policey;
#endif

};

#endif