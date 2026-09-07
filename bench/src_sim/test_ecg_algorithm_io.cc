#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

#include "graph.h"
#include "reader.h"

bool fail_next_array_allocation = false;

void* operator new[](std::size_t bytes) {
    if (fail_next_array_allocation) {
        fail_next_array_allocation = false;
        throw std::bad_alloc{};
    }
    void* pointer = std::malloc(bytes ? bytes : 1);
    if (!pointer)
        throw std::bad_alloc{};
    return pointer;
}
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

int main(int argc, char** argv) {
    if (argc != 3)
        return 2;
    try {
        const std::string path(argv[1]);
        if (std::string(argv[2]) == "weighted") {
            Reader<int32_t, NodeWeight<int32_t, int32_t>> reader(path);
            auto graph = reader.ReadSerializedGraph(true, 1024 * 1024);
            std::cout << graph.num_nodes() << ' ' << graph.num_edges_directed();
            for (int32_t vertex = 0; vertex < graph.num_nodes(); ++vertex)
                for (const auto& edge : graph.out_neigh(vertex))
                    std::cout << ' ' << edge.v << ':' << edge.w;
        } else {
            Reader<int32_t> reader(path);
            auto graph = reader.ReadSerializedGraph(true, 1024 * 1024);
            if (std::string(argv[2]) == "copy-failure") {
                std::vector<int32_t> ids(graph.num_nodes(), 0);
                const int32_t original = graph.get_org_id(0);
                fail_next_array_allocation = true;
                try {
                    graph.copy_org_ids(ids.data());
                    return 9;
                } catch (const std::bad_alloc&) {
                    if (graph.get_org_id(0) != original)
                        return 10;
                }
            }
            std::cout << graph.num_nodes() << ' ' << graph.num_edges_directed();
        }
        std::cout << '\n';
        return 0;
    } catch (const std::invalid_argument& error) {
        std::cerr << error.what() << '\n';
        return 3;
    } catch (const std::length_error& error) {
        std::cerr << error.what() << '\n';
        return 4;
    } catch (const std::ios_base::failure& error) {
        std::cerr << error.what() << '\n';
        return 5;
    }
}
