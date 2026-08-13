#pragma once

#include <cmath>

//#define WEIGHTED

namespace params {
    float damping = 0.15;
    unsigned int partition_size = (1024 * 1024)/sizeof(float);//(256*1024)/sizeof(float); //512kB cluster size is for cluster constructing
    unsigned int num_partitions = 0;
    unsigned int partition_offset = (unsigned)log2((float)partition_size); 
    unsigned int num_threads = 10;
    unsigned int overflow_ceil = 0;
};
