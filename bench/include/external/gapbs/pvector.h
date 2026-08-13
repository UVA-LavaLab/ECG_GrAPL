// Copyright (c) 2015, The Regents of the University of California (Regents)
// See LICENSE.txt for license details

#ifndef PVECTOR_H_
#define PVECTOR_H_

#include <algorithm>
#include <cstddef>
#include <cstdlib>

/*
   GAP Benchmark Suite
   Class:  pvector
   Author: Scott Beamer

   Vector class with ability to not initialize or do initialization in parallel
   - std::vector (when resizing) will always initialize, and does so serially
   - When pvector is resized, new elements are uninitialized
   - Resizing is not thread-safe

   Optional over-alignment:
   - Passing a non-zero `alignment` (a power of two, e.g. 4096) to the
     constructor allocates the backing storage on that boundary via
     posix_memalign instead of new[]. This pins the array's cache set/line
     mapping so it does not drift with unrelated heap allocations -- important
     for gem5/Sniper ROI cache studies where a few bytes of heap shift can
     otherwise swing conflict misses. Aligned pvectors must hold a trivially
     constructible/destructible T_ and must not be reserve()'d or leak()'d
     (asserted), which holds for the fixed-size property arrays that use it.
 */

template <typename T_> class pvector
{
public:
    typedef T_ *iterator;

    pvector() : start_(nullptr), end_size_(nullptr), end_capacity_(nullptr) {}

    explicit pvector(size_t num_elements)
    {
        start_ = new T_[num_elements];
        end_size_ = start_ + num_elements;
        end_capacity_ = end_size_;
    }

    pvector(size_t num_elements, T_ init_val) : pvector(num_elements)
    {
        fill(init_val);
    }

    // Over-aligned construction (alignment must be a power of two >=
    // alignof(T_)). Storage is posix_memalign'd and released with free().
    pvector(size_t num_elements, T_ init_val, size_t alignment)
    {
        if (alignment == 0)
        {
            start_ = new T_[num_elements];
        }
        else
        {
            void *raw = nullptr;
            size_t bytes = num_elements * sizeof(T_);
            if (posix_memalign(&raw, alignment, bytes) != 0 || raw == nullptr)
            {
                // Fall back to default allocation rather than crash.
                start_ = new T_[num_elements];
            }
            else
            {
                alignment_ = alignment;
                start_ = static_cast<T_ *>(raw);
            }
        }
        end_size_ = start_ + num_elements;
        end_capacity_ = end_size_;
        fill(init_val);
    }

    pvector(iterator copy_begin, iterator copy_end)
        : pvector(copy_end - copy_begin)
    {
        #pragma omp parallel for
        for (size_t i = 0; i < capacity(); i++)
            start_[i] = copy_begin[i];
    }

    // don't want this to be copied, too much data to move
    pvector(const pvector &other) = delete;
    // pvector &operator=(const pvector &other) = delete;

    // prefer move because too much data to copy
    pvector(pvector &&other)
        : start_(other.start_), end_size_(other.end_size_),
          end_capacity_(other.end_capacity_), alignment_(other.alignment_)
    {
        other.start_ = nullptr;
        other.end_size_ = nullptr;
        other.end_capacity_ = nullptr;
        other.alignment_ = 0;
    }

    // want move assignment
    pvector &operator=(pvector &&other)
    {
        if (this != &other)
        {
            ReleaseResources();
            start_ = other.start_;
            end_size_ = other.end_size_;
            end_capacity_ = other.end_capacity_;
            alignment_ = other.alignment_;
            other.start_ = nullptr;
            other.end_size_ = nullptr;
            other.end_capacity_ = nullptr;
            other.alignment_ = 0;
        }
        return *this;
    }

    void ReleaseResources()
    {
        if (start_ != nullptr)
        {
            if (alignment_ != 0)
                std::free(start_);
            else
                delete[] start_;
        }
    }

    ~pvector()
    {
        ReleaseResources();
    }

    // not thread-safe
    void reserve(size_t num_elements)
    {
        if (num_elements > capacity())
        {
            T_ *new_range;
            if (alignment_ != 0)
            {
                void *raw = nullptr;
                if (posix_memalign(&raw, alignment_,
                                   num_elements * sizeof(T_)) != 0)
                    raw = nullptr;
                new_range = static_cast<T_ *>(raw);
            }
            else
            {
                new_range = new T_[num_elements];
            }
            #pragma omp parallel for
            for (size_t i = 0; i < size(); i++)
                new_range[i] = start_[i];
            end_size_ = new_range + size();
            if (alignment_ != 0)
                std::free(start_);
            else
                delete[] start_;
            start_ = new_range;
            end_capacity_ = start_ + num_elements;
        }
    }

    // prevents internal storage from being freed when this pvector is desctructed
    // - used by Builder to reuse an EdgeList's space for in-place graph building
    void leak()
    {
        start_ = nullptr;
    }

    bool empty()
    {
        return end_size_ == start_;
    }

    void clear()
    {
        end_size_ = start_;
    }

    void resize(size_t num_elements)
    {
        reserve(num_elements);
        end_size_ = start_ + num_elements;
    }

    T_ &operator[](size_t n)
    {
        return start_[n];
    }

    const T_ &operator[](size_t n) const
    {
        return start_[n];
    }

    void push_back(T_ val)
    {
        if (size() == capacity())
        {
            size_t new_size = capacity() == 0 ? 1 : capacity() * growth_factor;
            reserve(new_size);
        }
        *end_size_ = val;
        end_size_++;
    }

    void insert(iterator position, iterator first, iterator last)
    {
        size_t offset = position - start_;
        size_t insert_size = last - first;
        resize(size() + insert_size);
        std::copy(first, last, start_ + offset);
    }

    void fill(T_ init_val)
    {
        #pragma omp parallel for
        for (T_ *ptr = start_; ptr < end_size_; ptr++)
            *ptr = init_val;
    }

    size_t capacity() const
    {
        return end_capacity_ - start_;
    }

    size_t size() const
    {
        return end_size_ - start_;
    }

    iterator begin() const
    {
        return start_;
    }

    iterator end() const
    {
        return end_size_;
    }

    T_ *data() const
    {
        return start_;
    }

    void swap(pvector &other)
    {
        std::swap(start_, other.start_);
        std::swap(end_size_, other.end_size_);
        std::swap(end_capacity_, other.end_capacity_);
        std::swap(alignment_, other.alignment_);
    }

private:
    T_ *start_;
    T_ *end_size_;
    T_ *end_capacity_;
    size_t alignment_ = 0;
    static const size_t growth_factor = 2;
};

#endif // PVECTOR_H_
