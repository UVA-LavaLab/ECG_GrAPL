#ifndef ALIGNED_ARRAY_H_
#define ALIGNED_ARRAY_H_

#include <cstddef>     // For size_t
#include <cstdlib>     // For std::aligned_alloc and std::free
#include <algorithm>   // For std::fill
#include <new>         // For std::bad_alloc
#include <iostream>    // For std::cout and std::endl
#include <string>      // For std::string
#include <vector>      // For std::vector

template <typename T>
struct AlignedArray
{
    T *data;
    size_t size;
    size_t alignment;
    size_t size_bytes; // New attribute for the total size in bytes

    AlignedArray(size_t size, size_t alignment = 4096, T init_val = T())
        : data(nullptr), size(size), alignment(alignment), size_bytes(size * sizeof(T))
    {
        data = static_cast<T *>(std::aligned_alloc(alignment, size_bytes));
        if (!data) throw std::bad_alloc();
        std::fill(data, data + size, init_val);  // Initialize memory with init_val
    }

    ~AlignedArray()
    {
        if (data != nullptr)
        {
            std::free(data);
        }
    }

    // Constructor for initializing from an std::vector
    AlignedArray(const std::vector<T> &vec, size_t alignment = 4096)
        : data(nullptr), size(vec.size()), alignment(alignment), size_bytes(vec.size() * sizeof(T))
    {
        data = static_cast<T *>(std::aligned_alloc(alignment, size_bytes));
        if (!data) throw std::bad_alloc();
        std::copy(vec.begin(), vec.end(), data);  // Copy elements from the vector
    }

    // Disable copy constructor and copy assignment operator
    AlignedArray(const AlignedArray &) = delete;
    AlignedArray &operator=(const AlignedArray &) = delete;

    // Enable move constructor and move assignment operator
    AlignedArray(AlignedArray &&other) noexcept
        : data(other.data), size(other.size), alignment(other.alignment), size_bytes(other.size_bytes)
    {
        other.data = nullptr;
        other.size = 0;
        other.alignment = 0;
        other.size_bytes = 0;
    }

    AlignedArray &operator=(AlignedArray &&other) noexcept
    {
        if (this != &other)
        {
            if (data != nullptr)
            {
                std::free(data);
            }
            data = other.data;
            size = other.size;
            alignment = other.alignment;
            size_bytes = other.size_bytes;
            other.data = nullptr;
            other.size = 0;
            other.alignment = 0;
            other.size_bytes = 0;
        }
        return *this;
    }

    // Overload operator[] for non-const access
    T &operator[](size_t index)
    {
        return data[index];
    }

    // Overload operator[] for const access
    const T &operator[](size_t index) const
    {
        return data[index];
    }

    // Function to display the contents of the array
    void display(const std::string &name) const
    {
        std::cout << name << ": ";
        for (size_t i = 0; i < size; ++i)
        {
            std::cout << data[i] << " ";
        }
        std::cout << std::endl;
    }
};

#endif // ALIGNED_ARRAY_H_
