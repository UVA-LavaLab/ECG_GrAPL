// -----------------------------------------------------------------------------
//
//      "00_AccelGraph"
//
// -----------------------------------------------------------------------------
// Copyright (c) 2014-2019 All rights reserved
// -----------------------------------------------------------------------------
// Author : Abdullah Mughrabi
// Email  : atmughra@ncsu.edu||atmughrabi@gmail.com
// File   : cache.c
// Create : 2019-06-29 12:31:24
// Revise : 2019-09-28 15:37:12
// Editor : Abdullah Mughrabi
// -----------------------------------------------------------------------------
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <omp.h>

#include "reorder.h"
#include "quantization.h"
#include "myMalloc.h"
#include "cache.h"


void initCacheLine(struct CacheLine *cacheLine)
{
    cacheLine->tag = 0;
    cacheLine->Flags = 0;
}
uint64_t getAddr(struct CacheLine *cacheLine)
{
    return cacheLine->addr;
}
uint64_t getTag(struct CacheLine *cacheLine)
{
    return cacheLine->tag;
}
uint8_t getFlags(struct CacheLine *cacheLine)
{
    return cacheLine->Flags;
}
uint64_t getSeq(struct CacheLine *cacheLine)
{
    return cacheLine->seq;
}
uint8_t getFreq(struct CacheLine *cacheLine)
{
    return cacheLine->freq;
}
uint8_t getRRPV(struct CacheLine *cacheLine)
{
    return cacheLine->RRPV;
}
uint8_t getSRRPV(struct CacheLine *cacheLine)
{
    return cacheLine->SRRPV;
}
uint8_t getPIN(struct CacheLine *cacheLine)
{
    return cacheLine->PIN;
}
uint8_t getPLRU(struct CacheLine *cacheLine)
{
    return cacheLine->PLRU;
}
uint8_t getXPRRPV(struct CacheLine *cacheLine)
{
    return cacheLine->XPRRPV;
}
uint32_t getPOPT(struct CacheLine *cacheLine)
{
    return cacheLine->POPT;
}

void setSeq(struct CacheLine *cacheLine, uint64_t Seq)
{
    cacheLine->seq = Seq;
}
void setFreq(struct CacheLine *cacheLine, uint8_t freq)
{
    cacheLine->freq = freq;
}
void setRRPV(struct CacheLine *cacheLine, uint8_t RRPV)
{
    cacheLine->RRPV = RRPV;
}
void setSRRPV(struct CacheLine *cacheLine, uint8_t SRRPV)
{
    cacheLine->SRRPV = SRRPV;
}
void setPIN(struct CacheLine *cacheLine, uint8_t PIN)
{
    cacheLine->PIN = PIN;
}
void setPLRU(struct CacheLine *cacheLine, uint8_t PLRU)
{
    cacheLine->PLRU = PLRU;
}
void setXPRRPV(struct CacheLine *cacheLine, uint8_t XPRRPV)
{
    cacheLine->XPRRPV = XPRRPV;
}

void setPOPT(struct CacheLine *cacheLine, uint32_t POPT)
{
    cacheLine->POPT = POPT;
}

void setFlags(struct CacheLine *cacheLine, uint8_t flags)
{
    cacheLine->Flags = flags;
}
void setTag(struct CacheLine *cacheLine, uint64_t a)
{
    cacheLine->tag = a;
}
void setAddr(struct CacheLine *cacheLine, uint64_t addr)
{
    cacheLine->addr = addr;
}
void invalidate(struct CacheLine *cacheLine)
{
    cacheLine->idx    = 0;
    cacheLine->seq    = 0;
    cacheLine->tag    = 0;//useful function
    cacheLine->Flags  = INVALID;
    cacheLine->RRPV   = RRPV_INIT;
    cacheLine->SRRPV  = SRRPV_INIT;
    cacheLine->PIN    = 0;
    cacheLine->PLRU   = 0;
    cacheLine->freq   = 0;
    cacheLine->XPRRPV = XPRRPV_INIT;
    cacheLine->POPT   = POPT_INIT;
}
uint32_t isValid(struct CacheLine *cacheLine)
{
    return ((cacheLine->Flags) != INVALID);
}


//cache helper functions

uint64_t calcTag(struct Cache *cache, uint64_t addr)
{
    return (addr >> (cache->log2Blk) );
}
uint64_t calcIndex(struct Cache *cache, uint64_t addr)
{
    return ((addr >> cache->log2Blk) & cache->tagMask);
}
uint64_t calcAddr4Tag(struct Cache *cache, uint64_t tag)
{
    return (tag << (cache->log2Blk));
}

uint64_t getRM(struct Cache *cache)
{
    return cache->readMisses;
}
uint64_t getWM(struct Cache *cache)
{
    return cache->writeMisses;
}
uint64_t getReads(struct Cache *cache)
{
    return cache->reads;
}
uint64_t getWrites(struct Cache *cache)
{
    return cache->writes;
}
uint64_t getWB(struct Cache *cache)
{
    return cache->writeBacks;
}
uint64_t getEVC(struct Cache *cache)
{
    return cache->evictions;
}
uint64_t getRMPrefetch(struct Cache *cache)
{
    return cache->readMissesPrefetch;
}
uint64_t getReadsPrefetch(struct Cache *cache)
{
    return cache->readsPrefetch;
}
void writeBack(struct Cache *cache, uint64_t addr)
{
    cache->writeBacks++;
}

// ********************************************************************************************
// ***************               Cache comparison                                **************
// ********************************************************************************************

struct CacheStructure *newCacheStructure(struct CacheStructureArguments *arguments, uint32_t num_vertices, uint32_t numPropertyRegions)
{
    struct CacheStructure *cache = (struct CacheStructure *) my_malloc(sizeof(struct CacheStructure));

    cache->ref_cache      = newCache( arguments->l1_size, arguments->l1_assoc, arguments->l1_blocksize, arguments->l1_prefetch_enable, num_vertices, arguments->l1_policy, numPropertyRegions);
    cache->ref_cache_l2   = newCache( arguments->l2_size, arguments->l2_assoc, arguments->l2_blocksize, arguments->l2_prefetch_enable, num_vertices, arguments->l2_policy, numPropertyRegions);
    cache->ref_cache_llc  = newCache( arguments->llc_size, arguments->llc_assoc, arguments->llc_blocksize, arguments->llc_prefetch_enable, num_vertices, arguments->llc_policy, numPropertyRegions);

    cache->ref_cache->cacheNext = cache->ref_cache_l2;
    cache->ref_cache_l2->cacheNext = cache->ref_cache_llc;
    cache->ref_cache_llc->cacheNext = NULL;

    return cache;
}

void initCacheStructureRegion(struct CacheStructure *cache, struct PropertyMetaData *propertyMetaData, uint32_t *offset_matrix, uint32_t *prefetch_matrix)
{
    cache->ref_cache->offset_matrix = offset_matrix;
    cache->ref_cache_l2->offset_matrix = offset_matrix;
    cache->ref_cache_llc->offset_matrix = offset_matrix;

    cache->ref_cache->prefetch_matrix = prefetch_matrix;
    cache->ref_cache_l2->prefetch_matrix = prefetch_matrix;
    cache->ref_cache_llc->prefetch_matrix = prefetch_matrix;
    initialzeCachePropertyRegions (cache->ref_cache, propertyMetaData, cache->ref_cache->size);
    initialzeCachePropertyRegions (cache->ref_cache_l2, propertyMetaData, cache->ref_cache_l2->size);
    initialzeCachePropertyRegions (cache->ref_cache_llc, propertyMetaData, cache->ref_cache_llc->size);
}

void freeCacheStructure(struct CacheStructure *cache)
{
    if(cache)
    {
        freeCache(cache->ref_cache);
        freeCache(cache->ref_cache_l2);
        freeCache(cache->ref_cache_llc);
        free(cache);
    }
}

// ********************************************************************************************
// ***************              general Cache functions                          **************
// ********************************************************************************************

struct Cache *newCache(uint32_t l1_size, uint32_t l1_assoc, uint32_t blocksize, uint32_t prefetch_enable, uint32_t num_vertices, uint32_t policy, uint32_t numPropertyRegions)
{
    uint64_t i;
    uint64_t j;

    struct Cache *cache = ( struct Cache *) my_malloc(sizeof(struct Cache));
    initCache(cache, l1_size, l1_assoc, blocksize, policy);

    cache->num_buckets         = 11;
    cache->numPropertyRegions  = numPropertyRegions;
    cache->propertyRegions     = (struct PropertyRegion *)my_malloc(sizeof(struct PropertyRegion) * numPropertyRegions);

    cache->thresholds              = (uint64_t *)my_malloc(sizeof(uint64_t) * cache->num_buckets );
    cache->thresholds_count        = (uint64_t *)my_malloc(sizeof(uint64_t) * cache->num_buckets );
    cache->thresholds_totalDegrees = (uint64_t *)my_malloc(sizeof(uint64_t) * cache->num_buckets );
    cache->thresholds_avgDegrees   = (uint64_t *)my_malloc(sizeof(uint64_t) * cache->num_buckets );
    cache->regions_avgDegrees      = (uint64_t **)my_malloc(sizeof(uint64_t *) * numPropertyRegions);
    cache->prefetch_enable         = prefetch_enable;

    cache->verticesMiss = NULL;
    cache->verticesHit  = NULL;
    cache->vertices_base_reuse  = NULL;
    cache->vertices_total_reuse = NULL;
    cache->vertices_accesses    = NULL;

    cache->vertices_base_reuse_region  = NULL;
    cache->vertices_total_reuse_region = NULL;
    cache->vertices_accesses_region    = NULL;

    cache->vertices_base_reuse_cl         = NULL;
    cache->vertices_total_reuse_cl        = NULL;
    cache->vertices_base_reuse_region_cl  = NULL;
    cache->vertices_total_reuse_region_cl = NULL;

    cache->offset_matrix = NULL;

    cache->threshold_accesses_region   = (uint64_t *)my_malloc(sizeof(uint64_t) * cache->num_buckets);

    for(i = 0; i < cache->numPropertyRegions; i++)
    {
        cache->regions_avgDegrees[i] = (uint64_t *)my_malloc(sizeof(uint64_t) * (cache->num_buckets + 1) );
        for(j = 0; j < (cache->num_buckets + 1); j++)
        {
            cache->regions_avgDegrees[i][j] = 0;
        }
    }

    for(i = 0; i < cache->num_buckets ; i++)
    {
        cache->thresholds[i]               = 0;
        cache->thresholds_count[i]         = 0;
        cache->thresholds_totalDegrees[i]  = 0;
        cache->thresholds_avgDegrees[i]    = 0;


        cache->threshold_accesses_region[i]   = 0;
    }

    for(i = 0; i < numPropertyRegions; i++)
    {
        cache->propertyRegions[i].upper_bound = 0;
        cache->propertyRegions[i].hot_bound   = 0;
        cache->propertyRegions[i].warm_bound  = 0;
        cache->propertyRegions[i].lower_bound = 0;
    }

    cache->numVertices  = num_vertices;

    uint32_t size_cl = 0;

    if(cache->numVertices)
    {
        size_cl = (num_vertices / (cache->lineSize / 4)) + 1;
        cache->verticesMiss = (uint64_t *)my_malloc(sizeof(uint64_t) * num_vertices);
        cache->verticesHit  = (uint64_t *)my_malloc(sizeof(uint64_t) * num_vertices);
        cache->vertices_base_reuse  = (uint64_t *)my_malloc(sizeof(uint64_t) * num_vertices);
        cache->vertices_total_reuse = (uint64_t *)my_malloc(sizeof(uint64_t) * num_vertices);
        cache->vertices_accesses    = (uint64_t *)my_malloc(sizeof(uint64_t) * num_vertices);

        cache->vertices_base_reuse_region  = (uint64_t *)my_malloc(sizeof(uint64_t) * num_vertices);
        cache->vertices_total_reuse_region = (uint64_t *)my_malloc(sizeof(uint64_t) * num_vertices);
        cache->vertices_accesses_region    = (uint64_t *)my_malloc(sizeof(uint64_t) * num_vertices);

        cache->vertices_base_reuse_cl         = (uint64_t *)my_malloc(sizeof(uint64_t) * size_cl);
        cache->vertices_total_reuse_cl        = (uint64_t *)my_malloc(sizeof(uint64_t) * size_cl);
        cache->vertices_base_reuse_region_cl  = (uint64_t *)my_malloc(sizeof(uint64_t) * size_cl);
        cache->vertices_total_reuse_region_cl = (uint64_t *)my_malloc(sizeof(uint64_t) * size_cl);
    }

    for(i = 0; i < num_vertices; i++)
    {
        cache->verticesMiss[i] = 0;
        cache->verticesHit[i] = 0;

        cache->vertices_base_reuse[i] = 0;
        cache->vertices_total_reuse[i] = 0;
        cache->vertices_accesses[i] = 0;

        cache->vertices_base_reuse_region[i]  = 0;
        cache->vertices_total_reuse_region[i] = 0;
        cache->vertices_accesses_region[i]    = 0;
    }

    for(i = 0; i < size_cl; i++)
    {
        cache->vertices_base_reuse_cl[i]   = 0;
        cache->vertices_total_reuse_cl[i]  = 0;
        cache->vertices_base_reuse_region_cl[i]     = 0;
        cache->vertices_total_reuse_region_cl[i]    = 0;
    }

    return cache;
}

void freeCache(struct Cache *cache)
{
    uint64_t i;

    if(cache)
    {
        if(cache->propertyRegions)
            free(cache->propertyRegions);
        if(cache->verticesMiss)
            free(cache->verticesMiss);
        if(cache->verticesHit)
            free(cache->verticesHit);
        if(cache->vertices_base_reuse)
            free(cache->vertices_base_reuse);
        if(cache->vertices_total_reuse)
            free(cache->vertices_total_reuse);
        if(cache->vertices_accesses)
            free(cache->vertices_accesses);
        if(cache->thresholds)
            free(cache->thresholds);
        if(cache->thresholds_count)
            free(cache->thresholds_count);
        if(cache->vertices_base_reuse_region)
            free(cache->vertices_base_reuse_region);
        if(cache->vertices_total_reuse_region)
            free(cache->vertices_total_reuse_region);
        if(cache->vertices_accesses_region)
            free(cache->vertices_accesses_region);
        if(cache->threshold_accesses_region)
            free(cache->threshold_accesses_region);
        if(cache->thresholds_totalDegrees)
            free(cache->thresholds_totalDegrees);
        if(cache->thresholds_avgDegrees)
            free(cache->thresholds_avgDegrees);

        if(cache->vertices_base_reuse_cl)
            free(cache->vertices_base_reuse_cl);
        if(cache->vertices_total_reuse_cl)
            free(cache->vertices_total_reuse_cl);
        if(cache->vertices_base_reuse_region_cl)
            free(cache->vertices_base_reuse_region_cl);
        if(cache->vertices_total_reuse_region_cl)
            free(cache->vertices_total_reuse_region_cl);

        for(i = 0; i < cache->numPropertyRegions; i++)
        {
            if(cache->regions_avgDegrees[i])
                free(cache->regions_avgDegrees[i]);
        }

        if(cache->regions_avgDegrees)
            free(cache->regions_avgDegrees);

        if(cache->cacheLines)
        {
            for(i = 0; i < cache->sets; i++)
            {
                if(cache->cacheLines[i])
                    free(cache->cacheLines[i]);
            }
            free(cache->cacheLines);
        }
        free(cache);
    }
}

void initCache(struct Cache *cache, int s, int a, int b, int p)
{
    uint64_t i, j;
    cache->reads = cache->readMisses = cache->readsPrefetch = cache->readMissesPrefetch = cache->writes = cache->evictions = 0;
    cache->writeMisses = cache->writeBacks = cache->currentCycle_preftcher = cache->currentCycle_cache = cache->currentCycle = 0;

    cache->policy     = (uint32_t)(p);
    cache->size       = (uint64_t)(s);
    cache->lineSize   = (uint64_t)(b);
    cache->assoc      = (uint64_t)(a);
    cache->sets       = (uint64_t)((s / b) / a);
    cache->numLines   = (uint64_t)(s / b);
    cache->log2Sets   = (uint64_t)(log2(cache->sets));
    cache->log2Blk    = (uint64_t)(log2(b));
    cache->access_counter    = 0;

    //*******************//
    //initialize your counters here//
    //*******************//

    cache->tagMask = 0;
    for(i = 0; i < cache->log2Sets; i++)
    {
        cache->tagMask <<= 1;
        cache->tagMask |= 1;
    }

    /**create a two dimentional cache, sized as cache[sets][assoc]**/

    cache->cacheLines = (struct CacheLine **) my_malloc(cache->sets * sizeof(struct CacheLine *));
    for(i = 0; i < cache->sets; i++)
    {
        cache->cacheLines[i] = (struct CacheLine *) my_malloc((cache->assoc + 1) * sizeof(struct CacheLine));
        for(j = 0; j < cache->assoc + 1; j++)
        {
            invalidate(&(cache->cacheLines[i][j]));
        }
    }
}

void online_cache_graph_stats(struct Cache *cache, uint32_t node)
{
    uint32_t first_Access = 0;
    uint32_t first_Access_region = 0;
    uint32_t first_Access_cl = 0;
    uint32_t first_Access_region_cl = 0;
    uint32_t node_cl = 0;
    uint32_t i;
    uint32_t threshold_idx = 0;

    cache->vertices_accesses[node]++;
    cache->access_counter++;
    node_cl = (node / (cache->lineSize / 4));

    // find threshhold index perbucket
    for ( i = 0; i < cache->num_buckets; ++i)
    {
        if(cache->degrees_pointer[node] <= cache->thresholds[i])
        {
            threshold_idx = i;
            break;
        }
    }

    cache->threshold_accesses_region[threshold_idx]++;

    if(cache->vertices_base_reuse[node] == 0)
        first_Access = 1;

    if(cache->vertices_base_reuse_region[node] == 0)
        first_Access_region = 1;

    if(cache->vertices_base_reuse_cl[node_cl] == 0)
        first_Access_cl = 1;

    if(cache->vertices_base_reuse_region_cl[node_cl] == 0)
        first_Access_region_cl = 1;

    if(first_Access)
    {
        cache->vertices_total_reuse[node] = 1;
    }
    else
    {
        cache->vertices_total_reuse[node] += (cache->access_counter - cache->vertices_base_reuse[node]);
    }

    if(first_Access_region)
    {
        cache->vertices_total_reuse_region[node] = 1;
    }
    else
    {
        cache->vertices_total_reuse_region[node] += (cache->threshold_accesses_region[threshold_idx] - cache->vertices_base_reuse_region[node]);
    }

    if(first_Access_cl)
    {
        cache->vertices_total_reuse_cl[node_cl] = 1;
    }
    else
    {
        cache->vertices_total_reuse_cl[node_cl] += (cache->access_counter - cache->vertices_base_reuse_cl[node_cl]);
    }

    if(first_Access_region_cl)
    {
        cache->vertices_total_reuse_region_cl[node_cl] = 1;
    }
    else
    {
        cache->vertices_total_reuse_region_cl[node_cl] += (cache->threshold_accesses_region[threshold_idx] - cache->vertices_base_reuse_region_cl[node_cl]);
    }

    cache->vertices_base_reuse[node]          = cache->access_counter;
    cache->vertices_base_reuse_region[node]   = cache->threshold_accesses_region[threshold_idx];

    cache->vertices_base_reuse_cl[node_cl]          = cache->access_counter;
    cache->vertices_base_reuse_region_cl[node_cl]   = cache->threshold_accesses_region[threshold_idx];

}


uint32_t checkInCache(struct Cache *cache, uint64_t addr)
{
    struct CacheLine *line = findLine(cache, addr);

    if(line == NULL)
        return 1;

    // updatePolicy(cache, findLine(cache, addr));
    // else
    // {
    //     struct CacheLine *lineLRU = getLRU(cache, addr);
    //     if(lineLRU == line)
    //         return 1;
    //     else
    //         return 0;
    // }

    return 0;
}

void Prefetch(struct Cache *cache, uint64_t addr, unsigned char op, uint32_t node, uint32_t mask, uint32_t vSrc, uint32_t vDst)
{
    cache->currentCycle++;/*per cache global counter to maintain LRU order
                             among cache ways, updated on every cache access*/
    cache->currentCycle_preftcher++;
    cache->readsPrefetch++;
    struct CacheLine *line = findLine(cache, addr);
    if(line == NULL)/*miss*/
    {
        cache->readMissesPrefetch++;
        fillLine(cache, addr, mask, vSrc, vDst);
    }
    else
    {
        /**since it's a hit, update LRU and update dirty flag**/
        updatePromotionPolicy(cache, line, mask, vSrc, vDst);
    }
}

// ********************************************************************************************
// ***************         AGING POLICIES                                        **************
// ********************************************************************************************

void updateAgingPolicy(struct Cache *cache)
{
    switch(cache->policy)
    {
    case LRU_POLICY:
        updateAgeLRU(cache);
        break;
    case GRASP_POLICY:
        updateAgeGRASP(cache);
        break;
    case LFU_POLICY:
        updateAgeLFU(cache);
        break;
    case GRASPXP_POLICY:
        updateAgeGRASPXP(cache);
        break;
    default:
        updateAgeLRU(cache);
    }
}

/*No aging for LRU*/
void updateAgeLRU(struct Cache *cache)
{

}

void updateAgeLFU(struct Cache *cache)
{
    uint64_t i, j;
    uint8_t freq = 0;
    for(i = 0; i < cache->sets; i++)
    {
        for(j = 0; j < cache->assoc; j++)
        {
            if(isValid(&(cache->cacheLines[i][j])))
            {
                freq = getFreq(&(cache->cacheLines[i][j]));
                if(freq > 0)
                    freq--;
                setFreq(&(cache->cacheLines[i][j]), freq);
            }
        }
    }
}

void updateAgeGRASPXP(struct Cache *cache)
{
    uint64_t i, j;
    uint8_t XPRRPV = 0;
    for(i = 0; i < cache->sets; i++)
    {
        for(j = 0; j < cache->assoc; j++)
        {
            if(isValid(&(cache->cacheLines[i][j])))
            {
                XPRRPV = getXPRRPV(&(cache->cacheLines[i][j]));
                if(XPRRPV > 0)
                    XPRRPV--;
                setXPRRPV(&(cache->cacheLines[i][j]), XPRRPV);
            }
        }
    }
}

/*No aging for GRASP*/
void updateAgeGRASP(struct Cache *cache)
{

}


// ********************************************************************************************
// ***************         INSERTION POLICIES                                    **************
// ********************************************************************************************

void updateInsertionPolicy(struct Cache *cache, struct CacheLine *line, uint32_t mask, uint32_t vSrc, uint32_t vDst)
{
    switch(cache->policy)
    {
    case LRU_POLICY:
        updateInsertLRU(cache, line);
        break;
    case LFU_POLICY:
        updateInsertLFU(cache, line);
        break;
    case GRASP_POLICY:
        updateInsertGRASP(cache, line);
        break;
    case SRRIP_POLICY:
        updateInsertSRRIP(cache, line);
        break;
    case PIN_POLICY:
        updateInsertPIN(cache, line);
        break;
    case PLRU_POLICY:
        updateInsertPLRU(cache, line);
        break;
    case GRASPXP_POLICY:
        updateInsertGRASPXP(cache, line);
        break;
    case MASK_POLICY:
        updateInsertMASK(cache, line, mask);
        break;
    case POPT_POLICY:
        updateInsertPOPT(cache, line, mask, vSrc, vDst);
        break;
    case GRASP_OPT_POLICY:
        updateInsertGRASPOPT(cache, line, mask, vSrc, vDst);
        break;
    default:
        updateInsertLRU(cache, line);
    }
}

/*upgrade LRU line to be MRU line*/
void updateInsertPOPT(struct Cache *cache, struct CacheLine *line, uint32_t mask, uint32_t vSrc, uint32_t vDst)
{
    if(inHotRegion(cache, line))
    {
        setRRPV(line, HOT_INSERT_RRPV);
    }
    else if (inWarmRegion(cache, line))
    {
        setRRPV(line, WARM_INSERT_RRPV);
    }
    else
    {
        setRRPV(line, DEFAULT_INSERT_RRPV);
    }

    uint8_t SRRPV = DEFAULT_INSERT_SRRPV;
    setSRRPV(line, SRRPV);

    if(vSrc != vDst)
        setPOPT(line, mask);

    setSeq(line, cache->currentCycle);
}

/*upgrade LRU line to be MRU line*/
void updateInsertGRASPOPT(struct Cache *cache, struct CacheLine *line, uint32_t mask, uint32_t vSrc, uint32_t vDst)
{
    if(inHotRegion(cache, line))
    {
        setRRPV(line, HOT_INSERT_RRPV);
    }
    else if (inWarmRegion(cache, line))
    {
        setRRPV(line, WARM_INSERT_RRPV);
    }
    else
    {
        setRRPV(line, DEFAULT_INSERT_RRPV);
    }

    uint8_t SRRPV = DEFAULT_INSERT_SRRPV;
    setSRRPV(line, SRRPV);

    if(vSrc != vDst)
        setPOPT(line, mask);

    setSeq(line, cache->currentCycle);
}


/*upgrade LRU line to be MRU line*/
void updateInsertLRU(struct Cache *cache, struct CacheLine *line)
{
    setSeq(line, cache->currentCycle);
}

void updateInsertLFU(struct Cache *cache, struct CacheLine *line)
{
    uint8_t freq = 0;
    setFreq(line, freq);
}

void updateInsertGRASP(struct Cache *cache, struct CacheLine *line)
{
    if(inHotRegion(cache, line))
    {
        setRRPV(line, HOT_INSERT_RRPV);
    }
    else if (inWarmRegion(cache, line))
    {
        setRRPV(line, WARM_INSERT_RRPV);
    }
    else
    {
        setRRPV(line, DEFAULT_INSERT_RRPV);
    }
}

void updateInsertMASK(struct Cache *cache, struct CacheLine *line, uint32_t mask)
{
    if(mask == VERTEX_VALUE_HOT)
    {
        setRRPV(line, HOT_INSERT_RRPV);
    }
    else if (mask == VERTEX_CACHE_WARM)
    {
        setRRPV(line, WARM_INSERT_RRPV);
    }
    else
    {
        setRRPV(line, DEFAULT_INSERT_RRPV);
    }
}

void updateInsertSRRIP(struct Cache *cache, struct CacheLine *line)
{
    uint8_t SRRPV = DEFAULT_INSERT_SRRPV;
    setSRRPV(line, SRRPV);
}

void updateInsertPIN(struct Cache *cache, struct CacheLine *line)
{
    if(inHotRegion(cache, line))
    {
        setSeq(line, cache->currentCycle);
        setPIN(line, 1);
    }
    else
    {
        setSeq(line, cache->currentCycle);
        setPIN(line, 0);
    }
}

void updateInsertPLRU(struct Cache *cache, struct CacheLine *line)
{
    uint64_t i, j, tag;
    uint8_t bit_sum = 0;
    uint8_t PLRU = 1;

    i      = calcIndex(cache, line->addr);
    tag = calcTag(cache, line->addr);


    for(j = 0; j < cache->assoc; j++)
    {
        if(getTag(&(cache->cacheLines[i][j])) != tag)
            bit_sum += getPLRU(&(cache->cacheLines[i][j]));
    }

    if(bit_sum == (cache->assoc - 1))
    {
        for(j = 0; j < (cache->assoc); j++)
        {
            setPLRU(&(cache->cacheLines[i][j]), 0);
        }
    }

    setPLRU(line, PLRU);
}

void updateInsertGRASPXP(struct Cache *cache, struct CacheLine *line)
{
    uint8_t XPRRPV = 0;
    XPRRPV = (uint8_t)getCacheRegionGRASPXP(cache, line);
    setXPRRPV(line, XPRRPV);
}

uint32_t inHotRegion(struct Cache *cache, struct CacheLine *line)
{
    uint32_t v;
    uint32_t result = 0;

    for (v = 0; v < cache->numPropertyRegions; ++v)
    {
        if((line->addr >=  cache->propertyRegions[v].lower_bound) && (line->addr < cache->propertyRegions[v].hot_bound))
        {
            result = 1;
        }
    }

    return result;
}

uint32_t inWarmRegion(struct Cache *cache, struct CacheLine *line)
{
    uint32_t v;
    uint32_t result = 0;

    for (v = 0; v < cache->numPropertyRegions; ++v)
    {
        if((line->addr >=  cache->propertyRegions[v].hot_bound) && (line->addr < cache->propertyRegions[v].warm_bound))
        {
            result = 1;
        }
    }
    return result;
}

uint32_t inHotRegionAddrGRASP(struct Cache *cache, uint64_t addr)
{
    uint32_t v;
    uint32_t result = 0;

    for (v = 0; v < cache->numPropertyRegions; ++v)
    {
        if((addr >=  cache->propertyRegions[v].lower_bound) && (addr < cache->propertyRegions[v].hot_bound))
        {
            result = 1;
        }
    }

    return result;
}

uint32_t inWarmRegionAddrGRASP(struct Cache *cache, uint64_t addr)
{
    uint32_t v;
    uint32_t result = 0;

    for (v = 0; v < cache->numPropertyRegions; ++v)
    {
        if((addr >=  cache->propertyRegions[v].hot_bound) && (addr < cache->propertyRegions[v].warm_bound))
        {
            result = 1;
        }
    }
    return result;
}


// ********************************************************************************************
// ***************         PROMOTION POLICIES                                    **************
// ********************************************************************************************

void updatePromotionPolicy(struct Cache *cache, struct CacheLine *line, uint32_t mask, uint32_t vSrc, uint32_t vDst)
{
    switch(cache->policy)
    {
    case LRU_POLICY:
        updatePromoteLRU(cache, line);
        break;
    case LFU_POLICY:
        updatePromoteLFU(cache, line);
        break;
    case GRASP_POLICY:
        updatePromoteGRASP(cache, line);
        break;
    case SRRIP_POLICY:
        updatePromoteSRRIP(cache, line);
        break;
    case PIN_POLICY:
        updatePromotePIN(cache, line);
        break;
    case PLRU_POLICY:
        updatePromotePLRU(cache, line);
        break;
    case GRASPXP_POLICY:
        updatePromoteGRASPXP(cache, line);
        break;
    case MASK_POLICY:
        updatePromoteMASK(cache, line, mask);
        break;
    case POPT_POLICY:
        updatePromotePOPT(cache, line, mask, vSrc, vDst);
        break;
    case GRASP_OPT_POLICY:
        updatePromotePOPT(cache, line, mask, vSrc, vDst);
        break;
    default:
        updatePromoteLRU(cache, line);
    }
}

void updatePromotePOPT(struct Cache *cache, struct CacheLine *line, uint32_t mask, uint32_t vSrc, uint32_t vDst)
{

    if(inHotRegion(cache, line))
    {
        setRRPV(line, HOT_HIT_RRPV);
    }
    else
    {
        uint8_t RRPV = getRRPV(line);
        if(RRPV > 0)
            RRPV--;
        setRRPV(line, RRPV);
    }

    setSRRPV(line, HIT_SRRPV);

    if(vSrc != vDst)
        setPOPT(line, mask);
    setSeq(line, cache->currentCycle);
}

void updatePromoteGRASPOPT(struct Cache *cache, struct CacheLine *line, uint32_t mask, uint32_t vSrc, uint32_t vDst)
{

    if(inHotRegion(cache, line))
    {
        setRRPV(line, HOT_HIT_RRPV);
    }
    else
    {
        uint8_t RRPV = getRRPV(line);
        if(RRPV > 0)
            RRPV--;
        setRRPV(line, RRPV);
    }

    setSRRPV(line, HIT_SRRPV);

    if(vSrc != vDst)
        setPOPT(line, mask);
    setSeq(line, cache->currentCycle);
}

/*upgrade LRU line to be MRU line*/
void updatePromoteLRU(struct Cache *cache, struct CacheLine *line)
{
    setSeq(line, cache->currentCycle);
}

void updatePromoteLFU(struct Cache *cache, struct CacheLine *line)
{
    uint8_t freq = getFreq(line);
    if(freq < FREQ_MAX)
        freq++;
    setFreq(line, freq);
}



void updatePromoteGRASP(struct Cache *cache, struct CacheLine *line)
{
    if(inHotRegion(cache, line))
    {
        setRRPV(line, HOT_HIT_RRPV);
    }
    else
    {
        uint8_t RRPV = getRRPV(line);
        if(RRPV > 0)
            RRPV--;
        setRRPV(line, RRPV);
    }
}

void updatePromoteMASK(struct Cache *cache, struct CacheLine *line, uint32_t mask)
{
    if(mask == VERTEX_VALUE_HOT)
    {
        setRRPV(line, HOT_HIT_RRPV);
    }
    else
    {
        uint8_t RRPV = getRRPV(line);
        if(RRPV > 0)
            RRPV--;
        setRRPV(line, RRPV);
    }
}

void updatePromoteSRRIP(struct Cache *cache, struct CacheLine *line)
{
    setSRRPV(line, HIT_SRRPV);
}

void updatePromotePIN(struct Cache *cache, struct CacheLine *line)
{
    setSeq(line, cache->currentCycle);
}

void updatePromotePLRU(struct Cache *cache, struct CacheLine *line)
{

    uint64_t i, j, tag;
    uint8_t bit_sum = 0;
    uint8_t PLRU = 1;

    i      = calcIndex(cache, line->addr);
    tag = calcTag(cache, line->addr);


    for(j = 0; j < cache->assoc; j++)
    {
        if(getTag(&(cache->cacheLines[i][j])) != tag)
            bit_sum += getPLRU(&(cache->cacheLines[i][j]));
    }

    if(bit_sum == (cache->assoc - 1))
    {
        for(j = 0; j < (cache->assoc); j++)
        {
            setPLRU(&(cache->cacheLines[i][j]), 0);
        }
    }

    setPLRU(line, PLRU);
}

void updatePromoteGRASPXP(struct Cache *cache, struct CacheLine *line)
{
    uint8_t XPRRPV = getXPRRPV(line);
    uint32_t v;
    uint32_t i;
    uint32_t avg;
    // uint32_t property_fraction = 100 / cache->numPropertyRegions; //classical vs ratio of array size in bytes

    for (v = 0; v < cache->numPropertyRegions; ++v)
    {
        for ( i = 1; i < (cache->num_buckets + 1); ++i)
        {
            if((line->addr >=  cache->regions_avgDegrees[v][i - 1]) && (line->addr < cache->regions_avgDegrees[v][i]))
            {
                avg = cache->thresholds_totalDegrees[i] / cache->thresholds_count[i];
                if(XPRRPV > avg)
                    XPRRPV -= avg;
                else
                    XPRRPV = 0;
                break;
            }
        }
    }

    setXPRRPV(line, XPRRPV);
}

// ********************************************************************************************
// ***************         VICTIM EVICTION POLICIES                              **************
// ********************************************************************************************

struct CacheLine *getVictimPolicy(struct Cache *cache, uint64_t addr, uint32_t mask, uint32_t vSrc, uint32_t vDst)
{
    struct CacheLine *victim = NULL;

    switch(cache->policy)
    {
    case LRU_POLICY:
        victim = getVictimLRU(cache, addr);
        break;
    case LFU_POLICY:
        victim = getVictimLFU(cache, addr);
        break;
    case GRASP_POLICY:
        victim = getVictimGRASP(cache, addr);
        break;
    case SRRIP_POLICY:
        victim = getVictimSRRIP(cache, addr);
        break;
    case PIN_POLICY:
        victim = getVictimPIN(cache, addr);
        break;
    case PLRU_POLICY:
        victim = getVictimPLRU(cache, addr);
        break;
    case GRASPXP_POLICY:
        victim = getVictimGRASPXP(cache, addr);
        break;
    case MASK_POLICY:
        victim = getVictimMASK(cache, addr);
        break;
    case POPT_POLICY:
        victim = getVictimPOPT(cache, addr, mask, vSrc, vDst);
        break;
    case GRASP_OPT_POLICY:
        victim = getVictimGRASPPOPT(cache, addr, mask, vSrc, vDst);
        break;
    default:
        victim = getVictimLRU(cache, addr);
    }

    return victim;
}

/*return an invalid line as LRU, if any, otherwise return LRU line*/
struct CacheLine *getVictimLRU(struct Cache *cache, uint64_t addr)
{
    uint64_t i, j, victim, min;

    victim = cache->assoc;
    min    = cache->currentCycle;
    i      = calcIndex(cache, addr);

    for(j = 0; j < cache->assoc; j++)
    {
        if(isValid(&(cache->cacheLines[i][j])) == 0)
        {
            cache->cacheLines[i][j].addr = addr;
            return &(cache->cacheLines[i][j]);
        }
    }
    for(j = 0; j < cache->assoc; j++)
    {
        if(getSeq(&(cache->cacheLines[i][j])) <= min)
        {
            victim = j;
            min = getSeq(&(cache->cacheLines[i][j]));
        }
    }
    assert(victim != cache->assoc);

    cache->evictions++;
    cache->cacheLines[i][victim].addr = addr;
    return &(cache->cacheLines[i][victim]);
}

struct CacheLine *getVictimPOPT(struct Cache *cache, uint64_t addr, uint32_t mask, uint32_t vSrc, uint32_t vDst)
{
    uint64_t i, j, victim, min, min2;
    uint64_t victim_multi, victim_multi_2;

    victim = cache->assoc;
    min    = POPT_MAX_REREF;
    i      = calcIndex(cache, addr);

    uint32_t *victim_cacheLines = (uint32_t *) my_malloc((cache->assoc + 1) * sizeof(uint32_t));

    for(j = 0; j < cache->assoc; j++)
    {
        if(vSrc != vDst)
            victim_cacheLines[j] = 0;
        else
            victim_cacheLines[j] = 1;

        if(isValid(&(cache->cacheLines[i][j])) == 0)
        {
            cache->cacheLines[i][j].addr = addr;
            return &(cache->cacheLines[i][j]);
        }
    }

    victim = 0;
    min = getPOPT(&(cache->cacheLines[i][0]));
    // min = 0;
    victim_multi = 0;
    victim_multi_2 = 0;

    // all ways contain irregData & frontier
    // find the line that is going to be accessed farthest into the future
    for(j = 1; j < cache->assoc; j++)
    {
        if(getPOPT(&(cache->cacheLines[i][j])) == min)
        {
            victim_multi++;
            victim_cacheLines[j] = 1;
            if(j == 1)
            {
                victim_cacheLines[j - 1] = 1;
            }
        }
        if(getPOPT(&(cache->cacheLines[i][j])) > min)
        {
            victim = j;
            min = getPOPT(&(cache->cacheLines[i][j]));
        }

    }
    assert(victim != cache->assoc);
    //To handle ties in reref dist we also factor in DRRIP information

    for(j = 0; j < cache->assoc; j++)
    {
        if(getPOPT(&(cache->cacheLines[i][j])) == POPT_MAX_REREF && getSRRPV(&(cache->cacheLines[i][j])) == SRRPV_INIT)
        {
            victim = j;
            victim_multi_2++;
            min = getPOPT(&(cache->cacheLines[i][j]));
        }
    }

    if(victim_multi_2 == cache->assoc)
    {
        victim = 0;
        min2 = getSRRPV(&(cache->cacheLines[i][0]));

        for(j = 1; j < cache->assoc; j++)
        {
            if(getSRRPV(&(cache->cacheLines[i][j])) > min2)
            {
                victim = j;
                min2 = getSRRPV(&(cache->cacheLines[i][j]));
            }
        }

        int diff = SRRPV_INIT - min2;
        for(j = 0; j < cache->assoc; j++)
        {
            if(victim_cacheLines[j] == 1)
            {
                uint8_t SRRPV = getSRRPV(&(cache->cacheLines[i][j])) + (diff);
                setSRRPV(&(cache->cacheLines[i][j]), SRRPV);
                assert(SRRPV <= SRRPV_INIT);
            }
        }

        assert(min2 != SRRPV_INIT || min2 != 0);
        assert(victim != cache->assoc);

    }
    else {
        if (min < POPT_MAX_REREF)
        {
            int diff = POPT_MAX_REREF - min;
            for(j = 0; j < cache->assoc; j++)
            {
                if(victim_cacheLines[j] == 0)
                {
                    uint8_t POPT = getPOPT(&(cache->cacheLines[i][j])) + (diff);
                    setPOPT(&(cache->cacheLines[i][j]), POPT);
                    assert(POPT <= POPT_MAX_REREF);
                }
            }
        }

        assert(min != POPT_MAX_REREF || min != 0);
        assert(victim != cache->assoc);
    }

    free(victim_cacheLines);
    cache->evictions++;
    cache->cacheLines[i][victim].addr = addr;
    return &(cache->cacheLines[i][victim]);
}

struct CacheLine *getVictimGRASPPOPT(struct Cache *cache, uint64_t addr, uint32_t mask, uint32_t vSrc, uint32_t vDst)
{
    uint64_t i, j, victim, min, min2;
    uint64_t victim_multi, victim_multi_2;

    victim = cache->assoc;
    min    = POPT_MAX_REREF;
    i      = calcIndex(cache, addr);

    uint32_t *victim_cacheLines = (uint32_t *) my_malloc((cache->assoc + 1) * sizeof(uint32_t));

    for(j = 0; j < cache->assoc; j++)
    {
        if(vSrc != vDst)
            victim_cacheLines[j] = 0;
        else
            victim_cacheLines[j] = 1;

        if(isValid(&(cache->cacheLines[i][j])) == 0)
        {
            cache->cacheLines[i][j].addr = addr;
            return &(cache->cacheLines[i][j]);
        }
    }

    victim = 0;
    min = getPOPT(&(cache->cacheLines[i][0]));
    // min = 0;
    victim_multi = 0;
    victim_multi_2 = 0;

    // all ways contain irregData & frontier
    // find the line that is going to be accessed farthest into the future
    for(j = 1; j < cache->assoc; j++)
    {
        if(getPOPT(&(cache->cacheLines[i][j])) == min)
        {
            victim_multi++;
            victim_cacheLines[j] = 1;
            if(j == 1)
            {
                victim_cacheLines[j - 1] = 1;
            }
        }
        if(getPOPT(&(cache->cacheLines[i][j])) > min)
        {
            victim = j;
            min = getPOPT(&(cache->cacheLines[i][j]));
        }

    }
    assert(victim != cache->assoc);
    //To handle ties in reref dist we also factor in DRRIP information

    for(j = 0; j < cache->assoc; j++)
    {
        if(getPOPT(&(cache->cacheLines[i][j])) == POPT_MAX_REREF && getRRPV(&(cache->cacheLines[i][j])) == DEFAULT_INSERT_RRPV)
        {
            victim = j;
            victim_multi_2++;
            min = getPOPT(&(cache->cacheLines[i][j]));
        }
    }

    if(victim_multi_2 == cache->assoc)
    {
        victim = 0;
        min2 = getRRPV(&(cache->cacheLines[i][0]));

        for(j = 1; j < cache->assoc; j++)
        {
            if(getRRPV(&(cache->cacheLines[i][j])) > min2)
            {
                victim = j;
                min2 = getRRPV(&(cache->cacheLines[i][j]));
            }
        }

        int diff = DEFAULT_INSERT_RRPV - min2;
        for(j = 0; j < cache->assoc; j++)
        {
            if(victim_cacheLines[j] == 1)
            {
                uint8_t RRPV = getRRPV(&(cache->cacheLines[i][j])) + (diff);
                setRRPV(&(cache->cacheLines[i][j]), RRPV);
                assert(RRPV <= DEFAULT_INSERT_RRPV);
            }
        }

        assert(min2 != DEFAULT_INSERT_RRPV || min2 != 0);
        assert(victim != cache->assoc);

    }
    else {
        if (min < POPT_MAX_REREF)
        {
            int diff = POPT_MAX_REREF - min;
            for(j = 0; j < cache->assoc; j++)
            {
                if(victim_cacheLines[j] == 0)
                {
                    uint8_t POPT = getPOPT(&(cache->cacheLines[i][j])) + (diff);
                    setPOPT(&(cache->cacheLines[i][j]), POPT);
                    assert(POPT <= POPT_MAX_REREF);
                }
            }
        }

        assert(min != POPT_MAX_REREF || min != 0);
        assert(victim != cache->assoc);
    }

    free(victim_cacheLines);
    cache->evictions++;
    cache->cacheLines[i][victim].addr = addr;
    return &(cache->cacheLines[i][victim]);
}

struct CacheLine *getVictimLFU(struct Cache *cache, uint64_t addr)
{
    uint64_t i, j, victim, min;

    victim = cache->assoc;
    min    = FREQ_MAX;
    i      = calcIndex(cache, addr);

    for(j = 0; j < cache->assoc; j++)
    {
        if(isValid(&(cache->cacheLines[i][j])) == 0)
        {
            cache->cacheLines[i][j].addr = addr;
            return &(cache->cacheLines[i][j]);
        }
    }
    for(j = 0; j < cache->assoc; j++)
    {
        if(getFreq(&(cache->cacheLines[i][j])) <= min)
        {
            victim = j;
            min = getFreq(&(cache->cacheLines[i][j]));
        }
    }
    assert(victim != cache->assoc);

    cache->evictions++;
    cache->cacheLines[i][victim].addr = addr;
    return &(cache->cacheLines[i][victim]);
}

struct CacheLine *getVictimGRASP(struct Cache *cache, uint64_t addr)
{
    uint64_t i, j, victim, min;

    victim = cache->assoc;
    min    = 0;
    i      = calcIndex(cache, addr);

    for(j = 0; j < cache->assoc; j++)
    {
        if(isValid(&(cache->cacheLines[i][j])) == 0)
        {
            cache->cacheLines[i][j].addr = addr;
            return &(cache->cacheLines[i][j]);
        }
    }

    victim = 0;
    min = getRRPV(&(cache->cacheLines[i][0]));

    for(j = 1; j < cache->assoc; j++)
    {
        if(getRRPV(&(cache->cacheLines[i][j])) > min)
        {
            victim = j;
            min = getRRPV(&(cache->cacheLines[i][j]));
        }
    }
    assert(victim != cache->assoc);

    // not in the GRASP paper optimizaiton
    if (min < DEFAULT_INSERT_RRPV)
    {
        int diff = DEFAULT_INSERT_RRPV - min;
        for(j = 0; j < cache->assoc; j++)
        {
            uint8_t RRPV = getRRPV(&(cache->cacheLines[i][j])) + diff;
            setRRPV(&(cache->cacheLines[i][j]), RRPV);
            assert(RRPV <= DEFAULT_INSERT_RRPV);
        }
    }

    cache->evictions++;
    cache->cacheLines[i][victim].addr = addr; // update victim with new address so we simulate hot/cold insertion
    return &(cache->cacheLines[i][victim]);
}

struct CacheLine *getVictimMASK(struct Cache *cache, uint64_t addr)
{
    uint64_t i, j, victim, min;

    victim = cache->assoc;
    min    = 0;
    i      = calcIndex(cache, addr);

    for(j = 0; j < cache->assoc; j++)
    {
        if(isValid(&(cache->cacheLines[i][j])) == 0)
        {
            cache->cacheLines[i][j].addr = addr;
            return &(cache->cacheLines[i][j]);
        }
    }

    victim = 0;
    min = getRRPV(&(cache->cacheLines[i][0]));

    for(j = 1; j < cache->assoc; j++)
    {
        if(getRRPV(&(cache->cacheLines[i][j])) > min)
        {
            victim = j;
            min = getRRPV(&(cache->cacheLines[i][j]));
        }
    }
    assert(victim != cache->assoc);

    // not in the GRASP paper optimizaiton
    if (min < DEFAULT_INSERT_RRPV)
    {
        int diff = DEFAULT_INSERT_RRPV - min;
        for(j = 0; j < cache->assoc; j++)
        {
            uint8_t RRPV = getRRPV(&(cache->cacheLines[i][j])) + diff;
            setRRPV(&(cache->cacheLines[i][j]), RRPV);
            assert(RRPV <= DEFAULT_INSERT_RRPV);
        }
    }

    cache->evictions++;
    cache->cacheLines[i][victim].addr = addr; // update victim with new address so we simulate hot/cold insertion
    return &(cache->cacheLines[i][victim]);
}


struct CacheLine *getVictimSRRIP(struct Cache *cache, uint64_t addr)
{
    uint64_t i, j, victim, min;

    victim = cache->assoc;
    min    = 0;
    i      = calcIndex(cache, addr);

    for(j = 0; j < cache->assoc; j++)
    {
        if(isValid(&(cache->cacheLines[i][j])) == 0)
        {
            cache->cacheLines[i][j].addr = addr;
            return &(cache->cacheLines[i][j]);
        }
    }

    victim = 0;
    min = getSRRPV(&(cache->cacheLines[i][0]));

    for(j = 1; j < cache->assoc; j++)
    {
        if(getSRRPV(&(cache->cacheLines[i][j])) > min)
        {
            victim = j;
            min = getSRRPV(&(cache->cacheLines[i][j]));
        }
    }
    assert(victim != cache->assoc);

    if (min < SRRPV_INIT)
    {
        int diff = SRRPV_INIT - min;
        for(j = 0; j < cache->assoc; j++)
        {
            uint8_t SRRPV = getSRRPV(&(cache->cacheLines[i][j])) + (diff);
            setSRRPV(&(cache->cacheLines[i][j]), SRRPV);
            assert(SRRPV <= SRRPV_INIT);
        }
    }

    assert(min != SRRPV_INIT || min != 0);
    assert(victim != cache->assoc);

    cache->evictions++;
    cache->cacheLines[i][victim].addr = addr;
    return &(cache->cacheLines[i][victim]);
}

/*return an invalid line as LRU, if any, otherwise return LRU line*/
struct CacheLine *getVictimPIN(struct Cache *cache, uint64_t addr)
{
    uint64_t i, j, victim, min;

    victim = cache->assoc;
    min    = cache->currentCycle;
    i      = calcIndex(cache, addr);

    for(j = 0; j < cache->assoc; j++)
    {
        if(isValid(&(cache->cacheLines[i][j])) == 0)
        {
            cache->cacheLines[i][j].addr = addr;
            return &(cache->cacheLines[i][j]);
        }
    }
    for(j = 0; j < cache->assoc; j++)
    {
        if(!getPIN(&(cache->cacheLines[i][j])) && (getSeq(&(cache->cacheLines[i][j])) <= min))
        {
            victim = j;
            min = getSeq(&(cache->cacheLines[i][j]));
        }
    }
    // assert(victim != cache->assoc);

    cache->evictions++;
    cache->cacheLines[i][victim].addr = addr; // update victim with new address so we simulate hot/cold insertion
    return &(cache->cacheLines[i][victim]);
}

/*return an invalid line as LRU, if any, otherwise return LRU line*/
uint8_t getVictimPINBypass(struct Cache *cache, uint64_t addr)
{
    uint64_t i, j, min;
    uint8_t bypass = 1;
    min    = cache->currentCycle;
    i      = calcIndex(cache, addr);

    for(j = 0; j < cache->assoc; j++)
    {
        if(isValid(&(cache->cacheLines[i][j])) == 0)
        {
            return 0;
        }
    }
    for(j = 0; j < cache->assoc; j++)
    {
        if(!getPIN(&(cache->cacheLines[i][j])) && (getSeq(&(cache->cacheLines[i][j])) <= min))
        {
            min = getSeq(&(cache->cacheLines[i][j]));
            bypass = 0;
        }
    }

    return bypass;
}


struct CacheLine *getVictimPLRU(struct Cache *cache, uint64_t addr)
{
    uint64_t i, j, victim;
    victim = cache->assoc;
    i      = calcIndex(cache, addr);

    for(j = 0; j < cache->assoc; j++)
    {
        if(isValid(&(cache->cacheLines[i][j])) == 0)
        {
            cache->cacheLines[i][j].addr = addr;
            return &(cache->cacheLines[i][j]);
        }
    }

    for(j = 0; j < cache->assoc; j++)
    {
        if(!getPLRU(&(cache->cacheLines[i][j])))
        {
            victim = j;
            break;
        }
    }
    assert(victim != cache->assoc);

    cache->evictions++;

    cache->cacheLines[i][victim].addr = addr;
    return &(cache->cacheLines[i][victim]);
}

struct CacheLine *getVictimGRASPXP(struct Cache *cache, uint64_t addr)
{
    uint64_t i, j, victim, min;

    victim = cache->assoc;
    min    = 0;
    i      = calcIndex(cache, addr);

    for(j = 0; j < cache->assoc; j++)
    {
        if(isValid(&(cache->cacheLines[i][j])) == 0)
        {
            cache->cacheLines[i][j].addr = addr;
            return &(cache->cacheLines[i][j]);
        }
    }

    victim = 0;
    min = getXPRRPV(&(cache->cacheLines[i][0]));

    for(j = 1; j < cache->assoc; j++)
    {
        if(getXPRRPV(&(cache->cacheLines[i][j])) > min)
        {
            victim = j;
            min = getXPRRPV(&(cache->cacheLines[i][j]));
        }
    }
    assert(victim != cache->assoc);

    if (min < XPRRPV_INIT)
    {
        int diff = XPRRPV_INIT - min;
        for(j = 0; j < cache->assoc; j++)
        {
            uint8_t XPRRPV = getXPRRPV(&(cache->cacheLines[i][j])) + diff;
            setXPRRPV(&(cache->cacheLines[i][j]), XPRRPV);
            assert(XPRRPV <= XPRRPV_INIT);
        }
    }

    assert(min != XPRRPV_INIT || min != 0);
    assert(victim != cache->assoc);

    cache->evictions++;
    cache->cacheLines[i][victim].addr = addr;
    return &(cache->cacheLines[i][victim]);
}

// ********************************************************************************************
// ***************         VICTIM PEEK POLICIES                                  **************
// ********************************************************************************************

struct CacheLine *peekVictimPolicy(struct Cache *cache, uint64_t addr)
{
    struct CacheLine *victim = NULL;

    switch(cache->policy)
    {
    case LRU_POLICY:
        victim = peekVictimLRU(cache, addr);
        break;
    case LFU_POLICY:
        victim = peekVictimLFU(cache, addr);
        break;
    case GRASP_POLICY:
        victim = peekVictimGRASP(cache, addr);
        break;
    case SRRIP_POLICY:
        victim = peekVictimSRRIP(cache, addr);
        break;
    case PIN_POLICY:
        victim = peekVictimPIN(cache, addr);
        break;
    case PLRU_POLICY:
        victim = peekVictimPLRU(cache, addr);
        break;
    case GRASPXP_POLICY:
        victim = peekVictimGRASPXP(cache, addr);
        break;
    case MASK_POLICY:
        victim = peekVictimMASK(cache, addr);
        break;
    default:
        victim = peekVictimLRU(cache, addr);
    }

    return victim;
}

/*return an invalid line as LRU, if any, otherwise return LRU line*/
struct CacheLine *peekVictimLRU(struct Cache *cache, uint64_t addr)
{
    uint64_t i, j, victim, min;

    victim = cache->assoc;
    min    = cache->currentCycle;
    i      = calcIndex(cache, addr);

    for(j = 0; j < cache->assoc; j++)
    {
        if(isValid(&(cache->cacheLines[i][j])) == 0)
        {
            cache->cacheLines[i][j].addr = addr;
            return &(cache->cacheLines[i][j]);
        }
    }
    for(j = 0; j < cache->assoc; j++)
    {
        if(getSeq(&(cache->cacheLines[i][j])) <= min)
        {
            victim = j;
            min = getSeq(&(cache->cacheLines[i][j]));
        }
    }
    assert(victim != cache->assoc);
    return &(cache->cacheLines[i][victim]);
}

struct CacheLine *peekVictimLFU(struct Cache *cache, uint64_t addr)
{
    uint64_t i, j, victim, min;

    victim = cache->assoc;
    min    = FREQ_MAX;
    i      = calcIndex(cache, addr);

    for(j = 0; j < cache->assoc; j++)
    {
        if(isValid(&(cache->cacheLines[i][j])) == 0)
        {
            return &(cache->cacheLines[i][j]);
        }
    }
    for(j = 0; j < cache->assoc; j++)
    {
        if(getFreq(&(cache->cacheLines[i][j])) <= min)
        {
            victim = j;
            min = getFreq(&(cache->cacheLines[i][j]));
        }
    }
    assert(victim != cache->assoc);
    return &(cache->cacheLines[i][victim]);
}

struct CacheLine *peekVictimGRASP(struct Cache *cache, uint64_t addr)
{
    uint64_t i, j, victim, min;

    victim = cache->assoc;
    min    = 0;
    i      = calcIndex(cache, addr);

    for(j = 0; j < cache->assoc; j++)
    {
        if(isValid(&(cache->cacheLines[i][j])) == 0)
        {
            return &(cache->cacheLines[i][j]);
        }
    }

    victim = 0;
    min = getRRPV(&(cache->cacheLines[i][0]));

    for(j = 1; j < cache->assoc; j++)
    {
        if(getRRPV(&(cache->cacheLines[i][j])) > min)
        {
            victim = j;
            min = getRRPV(&(cache->cacheLines[i][j]));
        }
    }
    assert(victim != cache->assoc);
    return &(cache->cacheLines[i][victim]);
}

struct CacheLine *peekVictimMASK(struct Cache *cache, uint64_t addr)
{
    uint64_t i, j, victim, min;

    victim = cache->assoc;
    min    = 0;
    i      = calcIndex(cache, addr);

    for(j = 0; j < cache->assoc; j++)
    {
        if(isValid(&(cache->cacheLines[i][j])) == 0)
        {
            return &(cache->cacheLines[i][j]);
        }
    }

    victim = 0;
    min = getRRPV(&(cache->cacheLines[i][0]));

    for(j = 1; j < cache->assoc; j++)
    {
        if(getRRPV(&(cache->cacheLines[i][j])) > min)
        {
            victim = j;
            min = getRRPV(&(cache->cacheLines[i][j]));
        }
    }
    assert(victim != cache->assoc);
    return &(cache->cacheLines[i][victim]);
}

struct CacheLine *peekVictimSRRIP(struct Cache *cache, uint64_t addr)
{
    uint64_t i, j, victim, min;

    victim = cache->assoc;
    min    = 0;
    i      = calcIndex(cache, addr);

    for(j = 0; j < cache->assoc; j++)
    {
        if(isValid(&(cache->cacheLines[i][j])) == 0)
        {
            return &(cache->cacheLines[i][j]);
        }
    }

    victim = 0;
    min = getSRRPV(&(cache->cacheLines[i][0]));

    for(j = 1; j < cache->assoc; j++)
    {
        if(getSRRPV(&(cache->cacheLines[i][j])) > min)
        {
            victim = j;
            min = getSRRPV(&(cache->cacheLines[i][j]));
        }
    }
    assert(victim != cache->assoc);
    return &(cache->cacheLines[i][victim]);
}

/*return an invalid line as LRU, if any, otherwise return LRU line*/
struct CacheLine *peekVictimPIN(struct Cache *cache, uint64_t addr)
{
    uint64_t i, j, victim, min;

    victim = cache->assoc;
    min    = cache->currentCycle;
    i      = calcIndex(cache, addr);

    for(j = 0; j < cache->assoc; j++)
    {
        if(isValid(&(cache->cacheLines[i][j])) == 0)
        {
            return &(cache->cacheLines[i][j]);
        }
    }
    for(j = 0; j < cache->assoc; j++)
    {
        if(!getPIN(&(cache->cacheLines[i][j])) && (getSeq(&(cache->cacheLines[i][j])) <= min))
        {
            victim = j;
            min = getSeq(&(cache->cacheLines[i][j]));
        }
    }
    // assert(victim != cache->assoc);
    return &(cache->cacheLines[i][victim]);
}


struct CacheLine *peekVictimPLRU(struct Cache *cache, uint64_t addr)
{
    uint64_t i, j, victim;
    victim = cache->assoc;
    i      = calcIndex(cache, addr);

    for(j = 0; j < cache->assoc; j++)
    {
        if(isValid(&(cache->cacheLines[i][j])) == 0)
        {
            return &(cache->cacheLines[i][j]);
        }
    }

    for(j = 0; j < cache->assoc; j++)
    {
        if(!getPLRU(&(cache->cacheLines[i][j])))
        {
            victim = j;
            break;
        }
    }
    assert(victim != cache->assoc);
    return &(cache->cacheLines[i][victim]);
}


struct CacheLine *peekVictimGRASPXP(struct Cache *cache, uint64_t addr)
{
    uint64_t i, j, victim, min;

    victim = cache->assoc;
    min    = 0;
    i      = calcIndex(cache, addr);

    for(j = 0; j < cache->assoc; j++)
    {
        if(isValid(&(cache->cacheLines[i][j])) == 0)
        {
            return &(cache->cacheLines[i][j]);
        }
    }

    victim = 0;
    min = getXPRRPV(&(cache->cacheLines[i][0]));

    for(j = 1; j < cache->assoc; j++)
    {
        if(getXPRRPV(&(cache->cacheLines[i][j])) > min)
        {
            victim = j;
            min = getXPRRPV(&(cache->cacheLines[i][j]));
        }
    }
    assert(victim != cache->assoc);
    return &(cache->cacheLines[i][victim]);
}


// ********************************************************************************************
// ***************         Cacheline lookups                                     **************
// ********************************************************************************************

/*look up line*/
struct CacheLine *findLine(struct Cache *cache, uint64_t addr)
{
    uint64_t i, j, tag, pos;

    pos = cache->assoc;
    tag = calcTag(cache, addr);
    i   = calcIndex(cache, addr);

    for(j = 0; j < cache->assoc; j++)
        if(isValid((&cache->cacheLines[i][j])))
            if(getTag(&(cache->cacheLines[i][j])) == tag)
            {
                pos = j;
                break;
            }
    if(pos == cache->assoc)
        return NULL;
    else
    {
        return &(cache->cacheLines[i][pos]);
    }
}


/*find a victim*/
struct CacheLine *findLineToReplace(struct Cache *cache, uint64_t addr, uint32_t mask, uint32_t vSrc, uint32_t vDst)
{
    struct CacheLine  *victim = getVictimPolicy(cache, addr, mask, vSrc, vDst);
    updateInsertionPolicy(cache, victim, mask, vSrc, vDst);

    return (victim);
}

/*allocate a new line*/
struct CacheLine *fillLine(struct Cache *cache, uint64_t addr, uint32_t mask, uint32_t vSrc, uint32_t vDst)
{
    uint64_t tag;

    struct CacheLine *victim = findLineToReplace(cache, addr, mask, vSrc, vDst);
    assert(victim != 0);
    if(getFlags(victim) == DIRTY)
    {
        writeBack(cache, addr);
    }

    tag = calcTag(cache, addr);
    setTag(victim, tag);
    setAddr(victim, addr);
    setFlags(victim, VALID);

    /**note that this cache line has been already
       upgraded to MRU in the previous function (findLineToReplace)**/

    return victim;
}


uint32_t minTwoIntegers(uint32_t num1, uint32_t num2)
{

    if(num1 < num2)
        return num1;
    else
        return num2;

}

uint32_t findRereferenceValPOPT(struct Cache *cache, int irregInd, int regInd)
{

    int vertexPerCacheLine = cache->lineSize / sizeof(uint32_t);
    int numCacheLines = (cache->numVertices + vertexPerCacheLine - 1) / vertexPerCacheLine;
    int epochSz = (cache->numVertices + POPT_NUM_EPOCH - 1) / POPT_NUM_EPOCH;
    int numSubEpochs = POPT_NUM_EPOCH / 2;
    int subEpochSz = (epochSz + numSubEpochs - 1) / numSubEpochs;

    // get vertex/cache line number
    int cacheLineID = irregInd / vertexPerCacheLine;
    int currEpoch   = regInd / epochSz;
    uint32_t mask    = 1;
    uint32_t orMask  = (mask << (POPT_BITS - 1));
    uint32_t andMask = (~orMask);

    if ((cache->offset_matrix[(currEpoch * numCacheLines) + cacheLineID] & orMask) != 0)
    {
        //We dont have a reference this epoch - Find next Epoch access
        uint32_t reref = cache->offset_matrix[(currEpoch * numCacheLines) + cacheLineID] & andMask;
        return reref;
    }
    else
    {
        //We have a reference this epoch
        uint32_t lastRefQ = cache->offset_matrix[(currEpoch * numCacheLines) + cacheLineID] & andMask;
        int delta = regInd - (currEpoch * epochSz);
        uint32_t subEpochDistQ = delta / subEpochSz;
        if (subEpochDistQ <= lastRefQ)
        {
            //We are yet to be accessed within this epoch
            return 0;
        }
        else
        {
            //No further accesses this epoch; Look beyond to the next Epoch
            uint32_t nextRef = cache->offset_matrix[((currEpoch + 1) * numCacheLines) + cacheLineID]; //this is why we store current & next Epoch information
            if ((nextRef & orMask) != 0)
            {
                uint32_t reref = nextRef & andMask;
                return (uint32_t)(minTwoIntegers(++reref, POPT_MAX_REREF));
            }
            else
            {
                //Going to be accessed next epoch
                return 1;
            }
        }
    }
}

void Access(struct Cache *cache, uint64_t addr, unsigned char op, uint32_t node, uint32_t mask, uint32_t vSrc, uint32_t vDst)
{
    if(node < cache->numVertices)
        online_cache_graph_stats(cache, node);
    cache->currentCycle++;
    /*per cache global counter to maintain LRU order among cache ways, updated on every cache access*/

    cache->currentCycle_cache++;

    if(op == 'w')
    {
        cache->writes++;
    }
    else if(op == 'r')
    {
        cache->reads++;

    }

    uint32_t popt_mask = POPT_MAX_REREF;
    popt_mask = findRereferenceValPOPT(cache, vDst, vSrc);

    if(cache->policy == POPT_POLICY || cache->policy == GRASP_OPT_POLICY)
    {
        mask = popt_mask;
        // if(mask != POPT_MAX_REREF)
        // printf("%u\n", mask);
    }


    uint32_t node_prefetch = cache->prefetch_matrix[node];
    uint64_t node_address = (addr - (node * 4)) + (node_prefetch * 4);
    if(checkInCache(cache,  node_address) && ENABLE_PREFETCH)
    {
        Prefetch(cache,  node_address, 'r', node_prefetch, mask, vSrc, vDst);
    }

    // if(popt_mask)
    //     printf("%u \n", popt_mask);

    struct CacheLine *line = findLine(cache, addr);
    if(line == NULL)/*miss*/
    {
        if(op == 'w')
        {
            cache->writeMisses++;
        }
        else if(op == 'r')
        {
            cache->readMisses++;
        }

        struct CacheLine *newline = NULL;

        if(cache->policy == PIN_POLICY)
        {
            if(!getVictimPINBypass(cache, addr))
            {
                newline = fillLine(cache, addr, mask, vSrc, vDst);
                newline->idx = node;
                if(op == 'w')
                    setFlags(newline, DIRTY);
            }
        }
        else
        {

            newline = fillLine(cache, addr, mask, vSrc, vDst);
            newline->idx = node;
            if(op == 'w')
                setFlags(newline, DIRTY);
        }

        if(node < cache->numVertices)
            cache->verticesMiss[node]++;
    }
    else
    {

        updatePromotionPolicy(cache, line, mask, vSrc, vDst);
        if(op == 'w')
            setFlags(line, DIRTY);

        if(node < cache->numVertices)
            cache->verticesHit[node]++;
    }
}

// ********************************************************************************************
// ***************               ACCElGraph Policy                               **************
// ********************************************************************************************
void AccessCacheStructureUInt32(struct CacheStructure *cache, uint64_t addr, unsigned char op, uint32_t node, uint32_t value, uint32_t vSrc, uint32_t vDst)
{
    Access(cache->ref_cache, addr, op, node, value, vSrc, vDst);
}

// ********************************************************************************************
// ***************               GRASP-XP Policy                                 **************
// ********************************************************************************************

uint64_t getCacheRegionGRASPXP(struct Cache *cache, struct CacheLine *line)
{
    uint32_t v;
    uint32_t i;
    // uint32_t property_fraction = 100 / cache->numPropertyRegions; //classical vs ratio of array size in bytes

    for (v = 0; v < cache->numPropertyRegions; ++v)
    {
        for ( i = 1; i < (cache->num_buckets + 1); ++i)
        {
            if((line->addr >=  cache->regions_avgDegrees[v][i - 1]) && (line->addr < cache->regions_avgDegrees[v][i]))
            {
                return cache->thresholds_avgDegrees[i - 1];
            }
        }
    }

    return 0;
}

uint64_t getCacheRegionAddrGRASPXP(struct Cache *cache, uint64_t addr)
{
    uint32_t v;
    uint32_t i;
    // uint32_t property_fraction = 100 / cache->numPropertyRegions; //classical vs ratio of array size in bytes

    for (v = 0; v < cache->numPropertyRegions; ++v)
    {
        for ( i = 1; i < (cache->num_buckets + 1); ++i)
        {
            if((addr >=  cache->regions_avgDegrees[v][i - 1]) && (addr < cache->regions_avgDegrees[v][i]))
            {
                return cache->thresholds_avgDegrees[i - 1];
            }
        }
    }

    return 0;
}


void setCacheRegionDegreeAvg(struct Cache *cache)
{
    uint32_t v;
    uint32_t i;
    // uint32_t property_fraction = 100 / cache->numPropertyRegions; //classical vs ratio of array size in bytes

    for (v = 0; v < cache->numPropertyRegions; ++v)
    {
        cache->regions_avgDegrees[v][0] = cache->propertyRegions[v].base_address;
        for ( i = 1; i < (cache->num_buckets + 1); ++i)
        {
            cache->regions_avgDegrees[v][i] = cache->regions_avgDegrees[v][i - 1] + (cache->thresholds_count[i - 1] * cache->propertyRegions[v].data_type_size);
        }
    }
}

void setCacheStructureThresholdDegreeAvg(struct CacheStructure *cache, uint32_t  *degrees)
{
    if(cache->ref_cache->numVertices)
    {
        setCacheThresholdDegreeAvg(cache->ref_cache, degrees);
    }

    if(cache->ref_cache_l2->numVertices)
    {
        setCacheThresholdDegreeAvg(cache->ref_cache_l2, degrees);
    }

    if(cache->ref_cache_llc->numVertices)
    {
        setCacheThresholdDegreeAvg(cache->ref_cache_llc, degrees);
    }
}

void setCacheThresholdDegreeAvg(struct Cache *cache, uint32_t  *degrees)
{
    uint32_t v;
    uint32_t i;
    uint64_t avgDegrees = 0;
    uint64_t totalDegrees = 0;
    float *thresholds_avgDegrees;
    cache->degrees_pointer = degrees;
    thresholds_avgDegrees    = (float *) my_malloc(cache->num_buckets * sizeof(float));

    for (v = 0; v < cache->numVertices; ++v)
    {
        avgDegrees +=  degrees[v];
    }

    avgDegrees /= cache->numVertices;

    // START initialize thresholds
    if(avgDegrees <= 1)
        cache->thresholds[0] = 1;
    else
        cache->thresholds[0] = (avgDegrees / 2);
    for ( i = 1; i < (cache->num_buckets - 1); ++i)
    {
        cache->thresholds[i] = cache->thresholds[i - 1] * 2;
    }
    cache->thresholds[cache->num_buckets - 1] = UINT32_MAX;
    // END initialize thresholds

    // collect stats perbucket
    for (v = 0; v < cache->numVertices; ++v)
    {
        for ( i = 0; i < cache->num_buckets; ++i)
        {
            if(degrees[v] <= cache->thresholds[i])
            {
                cache->thresholds_count[i] += 1;
                cache->thresholds_totalDegrees[i]  += degrees[v];
                break;
            }
        }
    }

    // collect stats perbucket
    for (v = 0; v < cache->numVertices; ++v)
    {
        totalDegrees += degrees[v];
    }


    for ( i = 0; i < cache->num_buckets; ++i)
    {
        if(cache->thresholds_count[i])
        {
            thresholds_avgDegrees[i] = XPRRPV_INIT * ((float)cache->thresholds_totalDegrees[i] / totalDegrees);
        }
        else
        {
            thresholds_avgDegrees[i] = 0;
        }
    }

    struct quant_params_8 rDivD_params;
    getMinMax_8(&rDivD_params, thresholds_avgDegrees, cache->num_buckets);
    rDivD_params.scale = GetScale_8(rDivD_params.min, rDivD_params.max);
    rDivD_params.zero = 0;

    for ( i = 0; i < cache->num_buckets; ++i)
    {
        cache->thresholds_avgDegrees[i]   =  quantize_8(thresholds_avgDegrees[i], rDivD_params.scale, rDivD_params.zero);
    }

    setCacheRegionDegreeAvg(cache);
    free(thresholds_avgDegrees);
}

// ********************************************************************************************
// ***************               GRASP Policy                                    **************
// ********************************************************************************************

// ********************************************************************************************
// ***************               GRASP Initializaiton                            **************
// ********************************************************************************************

void initialzeCachePropertyRegions (struct Cache *cache, struct PropertyMetaData *propertyMetaData, uint64_t size)
{
    uint32_t v;
    // uint64_t total_properties_size = 0;
    uint32_t property_fraction = 100 / cache->numPropertyRegions; //classical vs ratio of array size in bytes

    for (v = 0; v < cache->numPropertyRegions; ++v)
    {
        // total_properties_size += (propertyMetaData[v].size);
        cache->propertyRegions[v].base_address = propertyMetaData[v].base_address;
        cache->propertyRegions[v].size = propertyMetaData[v].size;
        cache->propertyRegions[v].data_type_size = propertyMetaData[v].data_type_size;
    }

    for (v = 0; v < cache->numPropertyRegions; ++v)
    {
        cache->propertyRegions[v].fraction    = property_fraction;// classical vs ratio of array size in bytes
        // cache->propertyRegions[v].fraction    = (uint64_t)(((uint64_t)(propertyMetaData[v].size) * 100) / total_properties_size );
        cache->propertyRegions[v].lower_bound = propertyMetaData[v].base_address;
        cache->propertyRegions[v].upper_bound = propertyMetaData[v].base_address + (uint64_t)(propertyMetaData[v].size);

        cache->propertyRegions[v].hot_bound = cache->propertyRegions[v].lower_bound + ((uint64_t)(size * cache->propertyRegions[v].fraction) / 100);
        if(cache->propertyRegions[v].hot_bound > cache->propertyRegions[v].upper_bound)
        {
            cache->propertyRegions[v].hot_bound = cache->propertyRegions[v].upper_bound;
        }

        cache->propertyRegions[v].warm_bound = cache->propertyRegions[v].hot_bound + ((uint64_t)(size * cache->propertyRegions[v].fraction) / 100);
        if(cache->propertyRegions[v].warm_bound > cache->propertyRegions[v].upper_bound)
        {
            cache->propertyRegions[v].warm_bound = cache->propertyRegions[v].upper_bound;
        }
    }
}

// ********************************************************************************************
// ***************               Stats output                                    **************
// ********************************************************************************************

float getMissRate(struct Cache *cache)
{
    float missRate = (double)((getWM(cache) + getRM(cache)) * 100) / (getReads(cache) + getWrites(cache)); //calculate miss rate
    missRate       = roundf(missRate * 100) / 100;

    return missRate;
}

void printStatsCache(struct Cache *cache)
{
    float missRate = (double)((getWM(cache) + getRM(cache)) * 100) / (getReads(cache) + getWrites(cache)); //calculate miss rate
    missRate       = roundf(missRate * 100) / 100;//rounding miss rate

    float missRateRead = (double)((getRM(cache)) * 100) / (getReads(cache)); //calculate miss rate
    missRateRead       = roundf(missRateRead * 100) / 100;//rounding miss rate

    float missRateWrite = (double)((getWM(cache)) * 100) / (getWrites(cache)); //calculate miss rate
    missRateWrite       = roundf(missRateWrite * 100) / 100;//rounding miss rate

    float missRatePrefetch = (double)(( getRMPrefetch(cache)) * 100) / (cache->currentCycle_preftcher); //calculate miss rate
    missRatePrefetch = roundf(missRatePrefetch * 100) / 100;

    printf(" -----------------------------------------------------\n");
    printf("| %-51s | \n", "Simulation results (Cache)");
    printf(" -----------------------------------------------------\n");

    switch(cache->policy)
    {
    case LRU_POLICY:
        printf("| %-51s | \n", "LRU_POLICY");
        break;
    case LFU_POLICY:
        printf("| %-51s | \n", "LFU_POLICY");
        break;
    case GRASP_POLICY:
        printf("| %-51s | \n", "GRASP_POLICY");
        break;
    case SRRIP_POLICY:
        printf("| %-51s | \n", "SRRIP_POLICY");
        break;
    case PIN_POLICY:
        printf("| %-51s | \n", "PIN_POLICY");
        break;
    case PLRU_POLICY:
        printf("| %-51s | \n", "PLRU_POLICY");
        break;
    case GRASPXP_POLICY:
        printf("| %-51s | \n", "GRASPXP_POLICY");
        break;
    case MASK_POLICY:
        printf("| %-51s | \n", "MASK_POLICY");
        break;
    case POPT_POLICY:
        printf("| %-51s | \n", "POPT_POLICY");
        break;
    case GRASP_OPT_POLICY:
        printf("| %-51s | \n", "GRASP_OPT_POLICY");
        break;
    default:
        printf("| %-51s | \n", "LRU_POLICY");
    }

    printf(" -----------------------------------------------------\n");
    printf("| %-21s | %'-27lu | \n", "Cache Size (KB)", cache->size / 1024 );
    printf("| %-21s | %'-27lu | \n", "Block Size",    cache->lineSize);
    printf("| %-21s | %'-27lu | \n", "Associativity", cache->assoc);
    printf(" -----------------------------------------------------\n");
    printf("| %-21s | %'-27lu | \n", "Reads/Writes", (getReads(cache) + getWrites(cache)) );
    printf("| %-21s | %'-27lu | \n", "Reads/Writes misses", (getWM(cache) + getRM(cache)));
    printf("| %-21s | %-27.2f | \n", "Miss rate(%)", missRate);
    printf(" -----------------------------------------------------\n");
    printf("| %-21s | %'-27lu | \n", "Reads", getReads(cache) );
    printf("| %-21s | %'-27lu | \n", "Read misses", getRM(cache) );
    printf("| %-21s | %-27.2f | \n", "Rd Miss rate(%)", missRateRead);
    printf(" -----------------------------------------------------\n");
    printf("| %-21s | %'-27lu | \n", "Writes", getWrites(cache) );
    printf("| %-21s | %'-27lu | \n", "Write misses", getWM(cache) );
    printf("| %-21s | %-27.2f | \n", "Wrt Miss rate(%)", missRateWrite);
    printf(" -----------------------------------------------------\n");
    printf("| %-21s | %'-27lu | \n", "Prefetch", getReadsPrefetch(cache) );
    printf("| %-21s | %'-27lu | \n", "Prefetch misses", getRMPrefetch(cache) );
    printf("| %-21s | %-27.2f | \n", "Prefetch Miss rate(%)", missRatePrefetch);
    printf(" -----------------------------------------------------\n");
    printf("| %-21s | %'-27lu | \n", "Writebacks", getWB(cache) );
    printf(" -----------------------------------------------------\n");
    printf("| %-21s | %'-27lu | \n", "Evictions", getEVC(cache) );
    printf(" -----------------------------------------------------\n");
}

void printStatsCacheToFile(struct Cache *cache, char *fname_perf)
{

    FILE *fptr1;
    fptr1 = fopen(fname_perf, "a+");

    float missRate = (double)((getWM(cache) + getRM(cache)) * 100) / (getReads(cache) + getWrites(cache)); //calculate miss rate
    missRate       = roundf(missRate * 100) / 100;//rounding miss rate

    float missRateRead = (double)((getRM(cache)) * 100) / (getReads(cache)); //calculate miss rate
    missRateRead       = roundf(missRateRead * 100) / 100;//rounding miss rate

    float missRateWrite = (double)((getWM(cache)) * 100) / (getWrites(cache)); //calculate miss rate
    missRateWrite       = roundf(missRateWrite * 100) / 100;//rounding miss rate

    fprintf(fptr1, " -----------------------------------------------------\n");
    fprintf(fptr1, "| %-51s | \n", "Simulation results (Cache)");
    fprintf(fptr1, " -----------------------------------------------------\n");

    switch(cache->policy)
    {
    case LRU_POLICY:
        fprintf(fptr1, "| %-51s | \n", "LRU_POLICY");
        break;
    case LFU_POLICY:
        printf("| %-51s | \n", "LFU_POLICY");
        break;
    case GRASP_POLICY:
        fprintf(fptr1, "| %-51s | \n", "GRASP_POLICY");
        break;
    case SRRIP_POLICY:
        fprintf(fptr1, "| %-51s | \n", "SRRIP_POLICY");
        break;
    case PIN_POLICY:
        fprintf(fptr1, "| %-51s | \n", "PIN_POLICY");
        break;
    case PLRU_POLICY:
        fprintf(fptr1, "| %-51s | \n", "PLRU_POLICY");
        break;
    case GRASPXP_POLICY:
        fprintf(fptr1, "| %-51s | \n", "GRASPXP_POLICY");
        break;
    case MASK_POLICY:
        fprintf(fptr1, "| %-51s | \n", "MASK_POLICY");
        break;
    case POPT_POLICY:
        fprintf(fptr1, "| %-51s | \n", "POPT_POLICY");
        break;
    case GRASP_OPT_POLICY:
        fprintf(fptr1, "| %-51s | \n", "GRASP_OPT_POLICY");
        break;
    default:
        fprintf(fptr1, "| %-51s | \n", "LRU_POLICY");
    }

    fprintf(fptr1, " -----------------------------------------------------\n");
    fprintf(fptr1, "| %-21s | %'-27lu | \n", "Cache Size (KB)", cache->size / 1024 );
    fprintf(fptr1, "| %-21s | %'-27lu | \n", "Block Size",    cache->lineSize);
    fprintf(fptr1, "| %-21s | %'-27lu | \n", "Associativity", cache->assoc);
    fprintf(fptr1, " -----------------------------------------------------\n");
    fprintf(fptr1, "| %-21s | %'-27lu | \n", "Reads/Writes", (getReads(cache) + getWrites(cache)) );
    fprintf(fptr1, "| %-21s | %'-27lu | \n", "Reads/Writes misses", (getWM(cache) + getRM(cache)));
    fprintf(fptr1, "| %-21s | %-27.2f | \n", "Miss rate(%)", missRate);
    fprintf(fptr1, " -----------------------------------------------------\n");
    fprintf(fptr1, "| %-21s | %'-27lu | \n", "Reads", getReads(cache) );
    fprintf(fptr1, "| %-21s | %'-27lu | \n", "Read misses", getRM(cache) );
    fprintf(fptr1, "| %-21s | %-27.2f | \n", "Rd Miss rate(%)", missRateRead);
    fprintf(fptr1, " -----------------------------------------------------\n");
    fprintf(fptr1, "| %-21s | %'-27lu | \n", "Writes", getWrites(cache) );
    fprintf(fptr1, "| %-21s | %'-27lu | \n", "Write misses", getWM(cache) );
    fprintf(fptr1, "| %-21s | %-27.2f | \n", "Wrt Miss rate(%)", missRateWrite);
    fprintf(fptr1, " -----------------------------------------------------\n");
    fprintf(fptr1, "| %-21s | %'-27lu | \n", "Writebacks", getWB(cache) );
    fprintf(fptr1, " -----------------------------------------------------\n");
    fprintf(fptr1, "| %-21s | %'-27lu | \n", "Evictions", getEVC(cache) );
    fprintf(fptr1, " -----------------------------------------------------\n");

    fclose(fptr1);
}


void printStatsGraphReuse(struct Cache *cache, uint32_t *degrees)
{
    uint32_t i = 0;
    uint32_t v = 0;
    uint64_t avgDegrees = 0;
    uint32_t num_buckets = cache->num_buckets;
    uint32_t num_vertices = cache->numVertices;

    if(!num_vertices)
        return;

    uint64_t *thresholds;
    uint64_t *thresholds_count;
    uint64_t *thresholds_totalAccesses;
    uint64_t *thresholds_totalDegrees;
    uint64_t *thresholds_totalReuses;
    uint64_t *thresholds_totalReuses_region;
    uint64_t *thresholds_totalMisses;

    float *thresholds_avgAccesses;
    float *thresholds_avgDegrees;
    float *thresholds_avgReuses;
    float *thresholds_avgReuses_region;
    float *thresholds_avgMisses;
    float Access_percentage;
    float Count_percentage;

    float *thresholds_avgReuses_cl;
    float *thresholds_avgReuses_region_cl;
    uint64_t *thresholds_totalReuses_cl;
    uint64_t *thresholds_totalReuses_region_cl;

    uint64_t thresholds_totalAccess  = 0;
    uint64_t thresholds_totalCount   = 0;
    uint64_t thresholds_totalDegree  = 0;
    uint64_t thresholds_totalReuse   = 0;
    uint64_t thresholds_totalReuse_region = 0;
    uint64_t thresholds_totalReuse_cl   = 0;
    uint64_t thresholds_totalReuse_region_cl = 0;
    uint64_t thresholds_totalMiss         = 0;

    thresholds               = (uint64_t *) my_malloc(num_buckets * sizeof(uint64_t));
    thresholds_count         = (uint64_t *) my_malloc(num_buckets * sizeof(uint64_t));
    thresholds_totalDegrees  = (uint64_t *) my_malloc(num_buckets * sizeof(uint64_t));
    thresholds_totalReuses   = (uint64_t *) my_malloc(num_buckets * sizeof(uint64_t));
    thresholds_totalReuses_region   = (uint64_t *) my_malloc(num_buckets * sizeof(uint64_t));
    thresholds_totalMisses   = (uint64_t *) my_malloc(num_buckets * sizeof(uint64_t));
    thresholds_totalAccesses = (uint64_t *) my_malloc(num_buckets * sizeof(uint64_t));
    thresholds_avgAccesses   = (float *) my_malloc(num_buckets * sizeof(float));
    thresholds_avgDegrees    = (float *) my_malloc(num_buckets * sizeof(float));
    thresholds_avgReuses     = (float *) my_malloc(num_buckets * sizeof(float));
    thresholds_avgReuses_region     = (float *) my_malloc(num_buckets * sizeof(float));
    thresholds_avgMisses     = (float *) my_malloc(num_buckets * sizeof(float));

    thresholds_avgReuses_cl            = (float *) my_malloc(num_buckets * sizeof(float));
    thresholds_avgReuses_region_cl     = (float *) my_malloc(num_buckets * sizeof(float));

    thresholds_totalReuses_cl          = (uint64_t *) my_malloc(num_buckets * sizeof(uint64_t));
    thresholds_totalReuses_region_cl   = (uint64_t *) my_malloc(num_buckets * sizeof(uint64_t));


    for (i = 0; i < num_buckets; ++i)
    {
        thresholds[i]               = 0;
        thresholds_count[i]         = 0;
        thresholds_totalDegrees[i]  = 0;
        thresholds_totalReuses[i]   = 0;
        thresholds_totalReuses_region[i]   = 0;
        thresholds_totalMisses[i]   = 0;
        thresholds_avgDegrees[i]    = 0.0f;
        thresholds_avgReuses[i]     = 0.0f;
        thresholds_avgReuses_region[i]     = 0.0f;
        thresholds_avgMisses[i]     = 0.0f;
        thresholds_totalAccesses[i] = 0;
        thresholds_avgAccesses[i]   = 0.0f;
        thresholds_avgReuses_cl[i]  = 0.0f;
        thresholds_avgReuses_region_cl[i]   = 0.0f;
        thresholds_totalReuses_cl[i]  = 0;
        thresholds_totalReuses_region_cl[i]  = 0;
    }

    for (v = 0; v < num_vertices; ++v)
    {
        avgDegrees +=  degrees[v];
    }

    avgDegrees /= num_vertices;

    // START initialize thresholds
    if(avgDegrees <= 1)
        thresholds[0] = 1;
    else
        thresholds[0] = (avgDegrees / 2);
    for ( i = 1; i < (num_buckets - 1); ++i)
    {
        thresholds[i] = thresholds[i - 1] * 2;
    }
    thresholds[num_buckets - 1] = UINT32_MAX;
    // END initialize thresholds


    // collect stats perbucket
    for (v = 0; v < num_vertices; ++v)
    {
        for ( i = 0; i < num_buckets; ++i)
        {
            if(degrees[v] <= thresholds[i])
            {
                if(cache->vertices_accesses[v])
                {
                    thresholds_count[i]            += 1;
                    thresholds_totalDegrees[i]     += degrees[v];
                }
                thresholds_totalReuses[i]           += cache->vertices_total_reuse[v];
                thresholds_totalReuses_region[i]    += cache->vertices_total_reuse_region[v];
                thresholds_totalReuses_cl[i]        += cache->vertices_total_reuse_cl[(v / (cache->lineSize / 4))];
                thresholds_totalReuses_region_cl[i] += cache->vertices_total_reuse_region_cl[(v / (cache->lineSize / 4))];
                thresholds_totalMisses[i]           += cache->verticesMiss[v];
                thresholds_totalAccesses[i]         += cache->vertices_accesses[v];
                break;
            }
        }
    }

    // collect stats perbucket
    for ( i = 0; i < num_buckets; ++i)
    {
        if(thresholds_count[i])
        {
            thresholds_avgDegrees[i]   = (float)thresholds_totalDegrees[i]  / thresholds_count[i];
            // thresholds_avgReuses[i]    = (float)thresholds_totalReuses[i]   / thresholds_totalAccesses[i];
            thresholds_avgMisses[i]    = (float)thresholds_totalMisses[i]    / thresholds_count[i];
            thresholds_avgAccesses[i]  = (float)thresholds_totalAccesses[i]  / thresholds_count[i];
        }

        if(thresholds_totalAccesses[i])
        {
            thresholds_avgReuses[i]           = (float)thresholds_totalReuses[i]          / thresholds_totalAccesses[i];
            thresholds_avgReuses_region[i]    = (float)thresholds_totalReuses_region[i]   / thresholds_totalAccesses[i];

            thresholds_avgReuses_cl[i]           = (float)thresholds_totalReuses_cl[i]          / thresholds_totalAccesses[i];
            thresholds_avgReuses_region_cl[i]    = (float)thresholds_totalReuses_region_cl[i]   / thresholds_totalAccesses[i];


            // printf("| %-15lu , %-15.2f , %-15lu , %-15lu |\n", thresholds[i], thresholds_avgReuses_region[i], thresholds_totalReuses_region[i], thresholds_totalAccesses[i]);

        }

        thresholds_totalAccess += thresholds_totalAccesses[i];
        thresholds_totalCount  += thresholds_count[i];
        thresholds_totalDegree += thresholds_totalDegrees[i];
        thresholds_totalReuse  += thresholds_totalReuses[i];
        thresholds_totalReuse_region     += thresholds_totalReuses_region[i];
        thresholds_totalReuse_cl         += thresholds_totalReuses_cl[i];
        thresholds_totalReuse_region_cl  += thresholds_totalReuses_region_cl[i];
        thresholds_totalMiss             += thresholds_totalMisses[i];
    }

    printf(" ----------------------------------------------------------------------------------------------------------------------------------------------------\n");
    printf("| %-15s | %-15s | %-15s | %-15s | %-15s | %-15s | %-20s | %-15s | \n", "<= Threshold", "Nodes(%)", "totalAccess(E)", "(%)Accesses", "avgDegrees", "avgReuse V/Axs", "avgReuse CL/Axs", "avgMisses");
    printf(" ----------------------------------------------------------------------------------------------------------------------------------------------------\n");
    for ( i = 0; i < num_buckets; ++i)
    {
        Access_percentage = 100 * (float)(thresholds_totalAccesses[i] / (float)thresholds_totalAccess);
        Count_percentage  = 100 * (float)(thresholds_count[i] / (float)num_vertices);
        printf("| %-15lu , %-15.2f , %-15lu , %-15.2f , %-15.2f , %-15.2f , %-20.2f , %-15.2f |\n", thresholds[i], Count_percentage, thresholds_totalAccesses[i], Access_percentage, thresholds_avgDegrees[i], thresholds_avgReuses[i], thresholds_avgReuses_cl[i], thresholds_avgMisses[i]);
    }
    printf(" ----------------------------------------------------------------------------------------------------------------------------------------------------\n");
    printf("| %-15s | %-15s | %-15s | %-15s | %-15s | %-15s | %-20s | %-15s | \n", "avgDegrees", "Total Count(V)", "totalAccess", "totalDegrees",  "avgDegrees", "totalReuse", "totalReuse_cl", "totalMisses");
    printf(" ----------------------------------------------------------------------------------------------------------------------------------------------------\n");
    printf("| %-15lu , %-15lu , %-15lu , %-15lu , %-15lu , %-15lu , %-20lu , %-15lu |\n", avgDegrees, thresholds_totalCount, thresholds_totalAccess, thresholds_totalDegree, avgDegrees, thresholds_totalReuse, thresholds_totalReuse_cl, thresholds_totalMiss);
    printf(" ----------------------------------------------------------------------------------------------------------------------------------------------------\n");

    free(thresholds);
    free(thresholds_count);
    free(thresholds_totalDegrees);
    free(thresholds_totalReuses);
    free(thresholds_totalReuses_region);
    free(thresholds_totalMisses);
    free(thresholds_avgDegrees);
    free(thresholds_avgReuses);
    free(thresholds_avgMisses);
    free(thresholds_totalAccesses);
    free(thresholds_avgAccesses);
    free(thresholds_avgReuses_cl);
    free(thresholds_avgReuses_region_cl);
    free(thresholds_totalReuses_cl);
    free(thresholds_totalReuses_region_cl);
}

void printStatsGraphCache(struct Cache *cache, uint32_t *in_degree, uint32_t *out_degree)
{
    printStatsCache(cache);

    if(cache->numVertices)
    {
        printf("\n======================  Reuse stats Out Degree =======================\n");
        printStatsGraphReuse(cache, out_degree);
        // printf("\n======================  Reuse stats In Degree  =======================\n");
        // printStatsGraphReuse(cache, in_degree);
    }
    uint32_t v;

    printf("\n=====================      Property Regions          =================\n");
    for (v = 0; v < cache->numPropertyRegions; ++v)
    {
        printf(" -----------------------------------------------------\n");
        printf("| %-25s | %-24u| \n", "ID", v);
        printf(" -----------------------------------------------------\n");
        printf("| %-25s | %-24u| \n", "size (Bytes)", cache->propertyRegions[v].size);
        printf("| %-25s | %-24u| \n", "data_type_size (Bytes)", cache->propertyRegions[v].data_type_size);
        printf("| %-25s | 0x%-22lx| \n", "base_address", cache->propertyRegions[v].base_address);
        printf(" -----------------------------------------------------\n");
        printf("| %-25s | %-24u| \n", "fraction", cache->propertyRegions[v].fraction );
        printf("| %-25s | 0x%-22lx| \n", "lower_bound", cache->propertyRegions[v].lower_bound );
        printf("| %-25s | 0x%-22lx| \n", "hot_bound", cache->propertyRegions[v].hot_bound );
        printf("| %-25s | 0x%-22lx| \n", "warm_bound", cache->propertyRegions[v].warm_bound );
        printf("| %-25s | 0x%-22lx| \n", "upper_bound", cache->propertyRegions[v].upper_bound );
        printf(" -----------------------------------------------------\n");
    }
}

void printStatsCacheStructure(struct CacheStructure *cache, uint32_t *in_degree, uint32_t *out_degree)
{

    printf("\n======================================================================\n");

    if(cache->ref_cache)
        printStatsGraphCache(cache->ref_cache, in_degree, out_degree);

    printf("\n======================================================================\n");

    // if(cache->ref_cache_l2)
    //     printStatsGraphCache(cache->ref_cache_l2, in_degree, out_degree);

    // printf("\n======================================================================\n");

    // if(cache->ref_cache_llc)
    //     printStatsGraphCache(cache->ref_cache_llc, in_degree, out_degree);

    // printf("\n======================================================================\n");

}

void printStatsCacheStructureToFile(struct CacheStructure *cache, char *fname_perf)
{
    if(cache->ref_cache)
        printStatsCacheToFile(cache->ref_cache, fname_perf);

    // if(cache->ref_cache_l2)
    //     printStatsCacheToFile(cache->ref_cache_l2, fname_perf);

    // if(cache->ref_cache_llc)
    //     printStatsCacheToFile(cache->ref_cache_llc, fname_perf);
}

void printStats(struct Cache *cache)
{
    float missRate = (double)((getWM(cache) + getRM(cache)) * 100) / (cache->currentCycle_cache); //calculate miss rate
    missRate = roundf(missRate * 100) / 100; //rounding miss rate

    float missRatePrefetch = (double)(( getRMPrefetch(cache)) * 100) / (cache->currentCycle_preftcher); //calculate miss rate
    missRatePrefetch = roundf(missRatePrefetch * 100) / 100;

    uint32_t v;
    uint32_t i;

    printf("\n -----------------------------------------------------\n");
    printf(" -----------------------------------------------------\n");
    printf("| %-51s | \n", "Simulation results (Cache)");
    printf(" -----------------------------------------------------\n");
    printf("| %-21s | %'-27lu | \n", "Reads", getReads(cache) );
    printf("| %-21s | %'-27lu | \n", "Read misses", getRM(cache) );
    printf(" -----------------------------------------------------\n");
    printf("| %-21s | %'-27lu | \n", "Writes", getWrites(cache) );
    printf("| %-21s | %'-27lu | \n", "Write misses", getWM(cache) );
    printf(" -----------------------------------------------------\n");
    printf("| %-21s | %-27.2f | \n", "Miss rate(%)", missRate);
    printf(" -----------------------------------------------------\n");
    printf("| %-21s | %'-27lu | \n", "Writebacks", getWB(cache) );
    printf(" -----------------------------------------------------\n");
    printf("| %-21s | %'-27lu | \n", "Evictions", getEVC(cache) );
    printf(" -----------------------------------------------------\n");


    if(cache->policy == GRASP_POLICY)
    {
        printf("\n=====================      Property Regions          =================\n");

        for (v = 0; v < cache->numPropertyRegions; ++v)
        {
            printf(" -----------------------------------------------------\n");
            printf("| %-25s | %-24u| \n", "ID", v);
            printf(" -----------------------------------------------------\n");
            printf("| %-25s | %-24u| \n", "size", cache->propertyRegions[v].size);
            printf("| %-25s | %-24u| \n", "data_type_size", cache->propertyRegions[v].data_type_size);
            printf("| %-25s | 0x%-22lx| \n", "base_address", cache->propertyRegions[v].base_address);
            printf(" -----------------------------------------------------\n");
            printf("| %-25s | %-24u| \n", "fraction", cache->propertyRegions[v].fraction );
            printf("| %-25s | 0x%-22lx| \n", "lower_bound", cache->propertyRegions[v].lower_bound );
            printf("| %-25s | 0x%-22lx| \n", "hot_bound", cache->propertyRegions[v].hot_bound );
            printf("| %-25s | 0x%-22lx| \n", "warm_bound", cache->propertyRegions[v].warm_bound );
            printf("| %-25s | 0x%-22lx| \n", "upper_bound", cache->propertyRegions[v].upper_bound );
            printf(" -----------------------------------------------------\n");
        }
    }

    double avgVerticesreuse  = 0;
    uint64_t numVerticesMiss   = 0;
    uint64_t totalVerticesMiss = 0;
    uint64_t accVerticesAccess = 0;
    // uint64_t   minReuse = 0;
    // uint32_t  maxVerticesMiss = 0;
    // uint32_t  maxNode = 0;


    for(i = 0; i < cache->numVertices; i++)
    {
        if(cache->verticesMiss[i] > 5)
        {
            numVerticesMiss++;
            totalVerticesMiss += cache->verticesMiss[i];
        }

        if(cache->vertices_accesses[i])
        {

            // printf("%u. Average reuse:             re %u acc %u\n", i,cache->vertices_total_reuse[i],cache->vertices_accesses[i]);

            avgVerticesreuse += cache->vertices_total_reuse[i] / cache->vertices_accesses[i];
            accVerticesAccess++;
        }

    }

    avgVerticesreuse /= accVerticesAccess;

    float MissNodesRatioReadMisses = (((double)numVerticesMiss / cache->numVertices) * 100.0);
    float ratioReadMissesMissNodes = (((double)totalVerticesMiss / getRM(cache)) * 100.0);

    printf("============ Graph Stats (Nodes cause highest miss stats) ============\n");
    printf("01. number of nodes:                          %lu\n", numVerticesMiss);
    printf("02. number of read misses:                    %lu\n", totalVerticesMiss);
    printf("03. ratio from total nodes :                  %.2f%%\n", MissNodesRatioReadMisses);
    printf("04. ratio from total read misses:             %.2f%%\n", ratioReadMissesMissNodes);
    printf("===================== Graph Stats (Other Stats) ======================\n");
    printf("05. Average reuse:             %.2f%%\n", avgVerticesreuse);

}
