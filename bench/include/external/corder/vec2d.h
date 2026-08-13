#pragma once

#include <iostream>
#include <vector>
#include <boost/multi_array.hpp>
#include <boost/smart_ptr.hpp>

#include "global.h"

template <typename T>
using array2d = typename boost::multi_array<T, 2>;

typedef boost::multi_array<unsigned,2> IntArray2d; 

template <typename T>
using Vector2d = typename std::vector<std::vector<T>>;


inline unsigned at(unsigned row, unsigned col) {
    return row * params::num_partitions + col;
}

unsigned get_segment_offset(unsigned index, std::vector<unsigned> offset) {
    unsigned rtn = 0;
    for(size_t i = 0; i < offset.size(); i++) {
        if(index < offset[i])
            break;
        else 
            rtn = i;
    }
    return rtn;
}