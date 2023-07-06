#ifndef GRAPHCONFIG_H
#define GRAPHCONFIG_H

#include <stdio.h>
#include <stdint.h>
#include "mt19937.h"

#define WEIGHTED 0
#define DIRECTED 1

// MYMALLOC_H
#define CACHELINE_BYTES 64
#define ALIGNED 1

#define VERTEX_T_SIZE 4
// #define VERTEX_T_SIZE 8

#define POPT_CACHE_U32 0xFFFFFFFF
#define POPT_CACHE_BITS 8

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

#define VERTEX_VALUE_MASK VERTEX_VALUE_MASK_U64
#define VERTEX_CACHE_MASK VERTEX_CACHE_MASK_U64

#define VERTEX_VALUE_HOT       VERTEX_VALUE_HOT_U64
#define VERTEX_CACHE_WARM      VERTEX_CACHE_WARM_U64
#define VERTEX_VALUE_LUKEWARM  VERTEX_VALUE_LUKEWARM_U64
#define VERTEX_CACHE_COLD      VERTEX_CACHE_COLD_U64

#else

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
    uint32_t binSize;
    uint32_t verbosity;
    uint32_t iterations;
    uint32_t trials;
    double epsilon;
    int source;
    uint32_t algorithm;
    uint32_t datastructure;
    uint32_t pushpull;
    uint32_t sort;
    uint32_t lmode; // reorder mode
    uint32_t lmode_l2; // reorder mode second layer
    uint32_t lmode_l3; // reorder mode third layer
    uint32_t mmode; // mask mode
    uint32_t symmetric;
    uint32_t weighted;
    uint32_t delta;
    int pre_numThreads;
    int algo_numThreads;
    int ker_numThreads;
    char *fnameb;
    char *fnamel;
    uint32_t fnameb_format;
    uint32_t convert_format;
    mt19937state mt19937var;

// #ifdef CACHE_HARNESS_META
    uint32_t l1_size;
    uint32_t l1_assoc;
    uint32_t l1_blocksize;
    uint32_t l1_policy;

    uint32_t l2_size;
    uint32_t l2_assoc;
    uint32_t l2_blocksize;
    uint32_t l2_policy;

    uint32_t llc_size;
    uint32_t llc_assoc;
    uint32_t llc_blocksize;
    uint32_t llc_policy;
    uint32_t popt_bits;
// #endif

};

#endif