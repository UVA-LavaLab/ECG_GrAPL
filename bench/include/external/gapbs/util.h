// Copyright (c) 2015, The Regents of the University of California (Regents)
// See LICENSE.txt for license details

#ifndef UTIL_H_
#define UTIL_H_

#include <cinttypes>
#include <iostream>
#include <stdexcept>
#include <stdio.h>
#include <stdlib.h>
#include <string>

#include "timer.h"

/*
GAP Benchmark Suite
Author: Scott Beamer

Miscellaneous helpers that don't fit into classes
*/

enum ReorderingAlgo
{
    ORIGINAL = 0,
    Random = 1,
    Sort = 2,
    HubSort = 3,
    HubCluster = 4,
    DBG = 5,
    HubSortDBG = 6,
    HubClusterDBG = 7,
    RabbitOrder = 8,        // -o 8:variant (csr=default, boost) | Native CSR is faster, no Boost deps
    GOrder = 9,
    COrder = 10,
    RCMOrder = 11,
    GraphBrewOrder = 12,    // -o 12:cluster:final:res:levels (leiden=default, rabbit, hubcluster)
    MAP = 13,               // Load reordering from file
    AdaptiveOrder = 14,     // ML-based perceptron selector
    // Leiden algorithms
    LeidenOrder = 15,       // -o 15:resolution (GVE-Leiden baseline)
    // Note: LeidenCSR (16) has been deprecated — GraphBrew (12) subsumes it.
    GoGraphOrder = 16,      // -o 16 (FEF-maximizing divide-and-conquer, P3 3.4)
    // RCM variants: -o 11 (GoGraph baseline), -o 11:bnf (CSR-native BNF)
};

static const int64_t kRandSeed = 27491095;

template <typename T> T *alloc_align_4k(size_t size)
{
    void *ptr = nullptr;
    // posix_memalign requires the memory address to be stored in a void pointer
    if (posix_memalign(&ptr, 4096, size * sizeof(T)) != 0)
    {
        ptr = nullptr; // If allocation fails, posix_memalign returns non-zero
    }
    return static_cast<T *>(ptr);
}

inline void PrintLabel(const std::string &label, const std::string &val)
{
    printf("%-21s%7s\n", (label + ":").c_str(), val.c_str());
}

inline void PrintTime(const std::string &s, double seconds)
{
    printf("%-21s%3.5lf\n", (s + ":").c_str(), seconds);
}

inline void PrintStep(const std::string &s, int64_t count)
{
    printf("%-14s%14" PRId64 "\n", (s + ":").c_str(), count);
}

inline void PrintStep(const std::string &s, double seconds, int64_t count = -1)
{
    if (count != -1)
        printf("%5s%11" PRId64 "  %10.5lf\n", s.c_str(), count, seconds);
    else
        printf("%5s%23.5lf\n", s.c_str(), seconds);
}

inline void PrintStep(int step, double seconds, int64_t count = -1)
{
    PrintStep(std::to_string(step), seconds, count);
}

// Runs op and prints the time it took to execute labelled by label
#define TIME_PRINT(label, op)                                                  \
  {                                                                            \
    Timer t_;                                                                  \
    t_.Start();                                                                \
    (op);                                                                      \
    t_.Stop();                                                                 \
    PrintTime(label, t_.Seconds());                                            \
  }

template <typename T_> class RangeIter
{
    T_ x_;

public:
    explicit RangeIter(T_ x) : x_(x) {}
    bool operator!=(RangeIter const &other) const
    {
        return x_ != other.x_;
    }
    T_ const &operator*() const
    {
        return x_;
    }
    RangeIter &operator++()
    {
        ++x_;
        return *this;
    }
};

template <typename T_> class Range
{
    T_ from_;
    T_ to_;

public:
    explicit Range(T_ to) : from_(0), to_(to) {}
    Range(T_ from, T_ to) : from_(from), to_(to) {}
    RangeIter<T_> begin() const
    {
        return RangeIter<T_>(from_);
    }
    RangeIter<T_> end() const
    {
        return RangeIter<T_>(to_);
    }
};

#endif // UTIL_H_
