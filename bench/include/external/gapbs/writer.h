// Copyright (c) 2015, The Regents of the University of California (Regents)
// See LICENSE.txt for license details

#ifndef WRITER_H_
#define WRITER_H_

#include <algorithm>
#include <fstream>
#include <iostream>
#include <string>
#include <type_traits>

#include "graph.h"

/*
   GAP Benchmark Suite
   Class:  Writer
   Author: Scott Beamer

   Given filename and graph, writes out the graph to storage
   - Should use WriteGraph(filename, serialized)
   - If serialized, will write out as serialized graph, otherwise, as edgelist
 */

template <typename NodeID_, typename DestID_ = NodeID_,
          typename WeightT_ = NodeID_>
class WriterBase
{
public:
    explicit WriterBase(CSRGraph<NodeID_, DestID_> &g) : g_(g) {}

    // Function to write data to the buffer and flush to file if needed
    void WriteToBuffer(std::fstream &out, std::vector<char> &buffer,
                       const std::string &data)
    {
        if (buffer.size() + data.size() > buffer.capacity())
        {
            out.write(buffer.data(), buffer.size());
            buffer.clear();
        }
        buffer.insert(buffer.end(), data.begin(), data.end());
    }

    void WriteEL(std::fstream &out)
    {
        const size_t buffer_size = 1024 * 1024; // 1024 KB buffer
        std::vector<char> buffer;
        buffer.reserve(buffer_size);

        for (NodeID_ u = 0; u < g_.num_nodes(); u++)
        {
            for (DestID_ v : g_.out_neigh(u))
            {
                if (g_.is_weighted())
                {
                    WriteToBuffer(
                        out, buffer,
                        std::to_string(u) + " " +
                        std::to_string(
                            static_cast<NodeWeight<NodeID_, WeightT_>>(v).v) +
                        " " +
                        std::to_string(
                            static_cast<NodeWeight<NodeID_, WeightT_>>(v).w) +
                        "\n");
                }
                else
                {
                    WriteToBuffer(out, buffer,
                                  std::to_string(u) + " " + std::to_string(v) + "\n");
                }
            }
        }

        if (!buffer.empty())
        {
            out.write(buffer.data(), buffer.size());
        }
    }

    void WriteGraphStructures(const std::string &filename)
    {
        bool directed = g_.directed();
        bool weighted = g_.is_weighted();

        // Files for out-structures
        std::fstream out_offset_out(ModifyFilename(filename, ".out_offset"), std::ios::out);
        std::fstream out_neigh_out(ModifyFilename(filename, ".out_neigh"), std::ios::out);
        std::fstream out_degree_out(ModifyFilename(filename, ".out_degree"), std::ios::out);
        std::fstream in_offset_out(ModifyFilename(filename, ".in_offset"), std::ios::out);
        std::fstream in_neigh_out(ModifyFilename(filename, ".in_neigh"), std::ios::out);
        std::fstream in_degree_out(ModifyFilename(filename, ".in_degree"), std::ios::out);
        std::fstream weights_out(ModifyFilename(filename, ".weights"), std::ios::out);

        size_t buffer_size = 1024 * 1024; // 1024 KB buffer
        std::vector<char> out_offset_buffer, out_neigh_buffer, out_degree_buffer, weights_buffer, in_offset_buffer, in_neigh_buffer, in_degree_buffer;
        out_offset_buffer.reserve(buffer_size);
        out_neigh_buffer.reserve(buffer_size);
        out_degree_buffer.reserve(buffer_size);
        in_offset_buffer.reserve(buffer_size);
        in_neigh_buffer.reserve(buffer_size);
        in_degree_buffer.reserve(buffer_size);
        weights_buffer.reserve(buffer_size);

        size_t out_offset = 0;
        size_t in_offset = 0;
        WriteToBuffer(out_neigh_out, out_neigh_buffer, std::to_string(g_.num_nodes()) + "\n");
        WriteToBuffer(out_neigh_out, out_neigh_buffer, std::to_string(g_.num_edges_directed()) + "\n");
        for (NodeID_ u = 0; u < g_.num_nodes(); u++)
        {
            // Write out-offsets
            WriteToBuffer(out_offset_out, out_offset_buffer, std::to_string(out_offset) + "\n");
            out_offset += g_.out_degree(u);

            // Write out-neighbors
            for (DestID_ v : g_.out_neigh(u))
            {
                WriteToBuffer(out_neigh_out, out_neigh_buffer, std::to_string(v) + "\n");
            }

            // Write out-degrees
            WriteToBuffer(out_degree_out, out_degree_buffer, std::to_string(g_.out_degree(u)) + "\n");

            // Write weights (if weighted)
            if (weighted)
            {
                for (DestID_ v : g_.out_neigh(u))
                {
                    WeightT_ weight = static_cast<NodeWeight<NodeID_, WeightT_>>(v).w;
                    WriteToBuffer(weights_out, weights_buffer, std::to_string(weight) + "\n");
                }
            }
            else
            {
                for (auto it = g_.out_neigh(u).begin(); it != g_.out_neigh(u).end(); ++it)
                {
                    WeightT_ weight = 1;
                    WriteToBuffer(weights_out, weights_buffer, std::to_string(weight) + "\n");
                }
            }

            // Write in-structures (if directed)
            if (directed)
            {
                WriteToBuffer(in_offset_out, in_offset_buffer, std::to_string(in_offset) + "\n");
                in_offset += g_.in_degree(u);

                for (DestID_ v : g_.in_neigh(u))
                {
                    WriteToBuffer(in_neigh_out, in_neigh_buffer, std::to_string(v) + "\n");
                }

                WriteToBuffer(in_degree_out, in_degree_buffer, std::to_string(g_.in_degree(u)) + "\n");
            }
            else
            {
                WriteToBuffer(in_offset_out, in_offset_buffer, std::to_string(in_offset) + "\n");
                in_offset += g_.out_degree(u);

                for (DestID_ v : g_.out_neigh(u))
                {
                    WriteToBuffer(in_neigh_out, in_neigh_buffer, std::to_string(v) + "\n");
                }

                WriteToBuffer(in_degree_out, in_degree_buffer, std::to_string(g_.out_degree(u)) + "\n");
            }
        }

        // Flush remaining buffer content
        if (!out_offset_buffer.empty()) out_offset_out.write(out_offset_buffer.data(), out_offset_buffer.size());
        if (!out_neigh_buffer.empty()) out_neigh_out.write(out_neigh_buffer.data(), out_neigh_buffer.size());
        if (!out_degree_buffer.empty()) out_degree_out.write(out_degree_buffer.data(), out_degree_buffer.size());
        if (!in_offset_buffer.empty()) in_offset_out.write(in_offset_buffer.data(), in_offset_buffer.size());
        if (!in_neigh_buffer.empty()) in_neigh_out.write(in_neigh_buffer.data(), in_neigh_buffer.size());
        if (!in_degree_buffer.empty()) in_degree_out.write(in_degree_buffer.data(), in_degree_buffer.size());
        if (!weights_buffer.empty()) weights_out.write(weights_buffer.data(), weights_buffer.size());

        out_offset_out.close();
        out_neigh_out.close();
        out_degree_out.close();
        in_offset_out.close();
        in_neigh_out.close();
        in_degree_out.close();
        weights_out.close();
    }

    void WriteLIGRA(std::fstream &out)
    {
        const size_t buffer_size = 1024 * 1024; // 1024 KB buffer
        std::vector<char> buffer;
        buffer.reserve(buffer_size);

        long n = g_.num_nodes();
        long m = g_.num_edges_directed();

        // Write the header
        if (g_.is_weighted())
        {
            WriteToBuffer(out, buffer, "WeightedAdjacencyGraph\n");
        }
        else
        {
            WriteToBuffer(out, buffer, "AdjacencyGraph\n");
        }
        WriteToBuffer(out, buffer, std::to_string(n) + "\n");
        WriteToBuffer(out, buffer, std::to_string(m) + "\n");

        // Write the offsets
        long offset = 0;
        for (NodeID_ u = 0; u < g_.num_nodes(); ++u)
        {
            WriteToBuffer(out, buffer, std::to_string(offset) + "\n");
            offset += g_.out_neigh(u).end() - g_.out_neigh(u).begin();
        }

        for (NodeID_ u = 0; u < g_.num_nodes(); u++)
        {
            for (DestID_ v : g_.out_neigh(u))
            {
                if (g_.is_weighted())
                {
                    WriteToBuffer(
                        out, buffer,
                        std::to_string(static_cast<NodeWeight<NodeID_, WeightT_>>(v).v) +
                        "\n"); // 0-based indexing
                }
                else
                {
                    WriteToBuffer(out, buffer,
                                  std::to_string(v) + "\n"); // 0-based indexing
                }
            }
        }

        // Write the weights if the graph is weighted
        if (g_.is_weighted())
        {
            for (NodeID_ u = 0; u < g_.num_nodes(); ++u)
            {
                for (DestID_ v : g_.out_neigh(u))
                {
                    WriteToBuffer(
                        out, buffer,
                        std::to_string(static_cast<NodeWeight<NodeID_, WeightT_>>(v).w) +
                        "\n"); // 0-based indexing
                }
            }
        }

        if (!buffer.empty())
        {
            out.write(buffer.data(), buffer.size());
        }
    }

    void WriteMTX(std::fstream &out)
    {
        const size_t buffer_size = 1024 * 1024; // 1024 KB buffer
        std::vector<char> buffer;
        buffer.reserve(buffer_size);

        std::string field_type;
        std::string symmetry_type;
        if (g_.is_weighted())
        {
            if (std::is_integral<WeightT_>::value)
            {
                field_type = "integer";
            }
            else if (std::is_floating_point<WeightT_>::value)
            {
                field_type = "real";
            }
            else
            {
                std::cerr << "Unsupported weight type." << std::endl;
                return;
            }
        }
        else
        {
            field_type = "pattern";
        }

        if (g_.directed())
        {
            symmetry_type = "general";
        }
        else
        {
            symmetry_type = "symmetric";
        }

        // Write the header
        WriteToBuffer(out, buffer,
                      "%%MatrixMarket matrix coordinate " + field_type + " " +
                      symmetry_type + "\n");
        WriteToBuffer(out, buffer, "%\n");
        WriteToBuffer(out, buffer,
                      std::to_string(g_.num_nodes()) + " " +
                      std::to_string(g_.num_nodes()) + " " +
                      std::to_string(g_.num_edges_directed()) + "\n");

        // Write the edges
        for (NodeID_ u = 0; u < g_.num_nodes(); u++)
        {
            for (DestID_ v : g_.out_neigh(u))
            {
                if (g_.is_weighted())
                {
                    WriteToBuffer(
                        out, buffer,
                        std::to_string(u + 1) + " " +
                        std::to_string(
                            static_cast<NodeWeight<NodeID_, WeightT_>>(v).v + 1) +
                        " " +
                        std::to_string(
                            static_cast<NodeWeight<NodeID_, WeightT_>>(v).w) +
                        "\n");
                }
                else
                {
                    WriteToBuffer(out, buffer,
                                  std::to_string(u + 1) + " " + std::to_string(v + 1) +
                                  "\n");
                }
            }
        }

        // Write any remaining data in the buffer
        if (!buffer.empty())
        {
            out.write(buffer.data(), buffer.size());
        }
    }

    void WriteSerializedGraph(std::fstream &out)
    {
        if (!std::is_same<NodeID_, SGID>::value)
        {
            std::cout << "serialized graphs only allowed for 32b IDs" << std::endl;
            std::exit(-4);
        }
        if (!std::is_same<DestID_, NodeID_>::value &&
                !std::is_same<DestID_, NodeWeight<NodeID_, SGID>>::value)
        {
            std::cout << ".wsg only allowed for int32_t weights" << std::endl;
            std::exit(-8);
        }
        bool directed = g_.directed();
        SGOffset num_nodes = g_.num_nodes();
        SGOffset edges_to_write = g_.num_edges_directed();
        std::streamsize index_bytes = (num_nodes + 1) * sizeof(SGOffset);
        std::streamsize neigh_bytes;
        std::streamsize ids_bytes = num_nodes * sizeof(NodeID_);

        if (std::is_same<DestID_, NodeID_>::value)
            neigh_bytes = edges_to_write * sizeof(SGID);
        else
            neigh_bytes = edges_to_write * sizeof(NodeWeight<NodeID_, SGID>);
        out.write(reinterpret_cast<char *>(&directed), sizeof(bool));
        out.write(reinterpret_cast<char *>(&edges_to_write), sizeof(SGOffset));
        out.write(reinterpret_cast<char *>(&num_nodes), sizeof(SGOffset));
        pvector<SGOffset> offsets = g_.VertexOffsets(false);
        out.write(reinterpret_cast<char *>(offsets.data()), index_bytes);
        out.write(reinterpret_cast<char *>(g_.out_neigh(0).begin()), neigh_bytes);
        if (directed)
        {
            offsets = g_.VertexOffsets(true);
            out.write(reinterpret_cast<char *>(offsets.data()), index_bytes);
            out.write(reinterpret_cast<char *>(g_.in_neigh(0).begin()), neigh_bytes);
        }
        // Write original IDs
        NodeID_ *temp_org_ids = g_.get_org_ids();
        out.write(reinterpret_cast<char *>(temp_org_ids), ids_bytes);

        out.flush();
        out.close();
    }

    void WriteSerializedLabels(std::fstream &out)
    {
        if (!std::is_same<NodeID_, SGID>::value)
        {
            std::cout << "serialized graphs only allowed for 32b IDs" << std::endl;
            std::exit(-4);
        }
        SGOffset num_nodes = g_.num_nodes();
        std::streamsize ids_bytes = num_nodes * sizeof(NodeID_);
        out.write(reinterpret_cast<char *>(g_.get_org_ids()), ids_bytes);
    }

    void WriteListLabels(std::fstream &out)
    {
        NodeID_ *source_array = g_.get_org_ids();
        for (NodeID_ u = 0; u < g_.num_nodes(); u++)
        {
            out << source_array[u] << std::endl;
        }
    }

    // Helper function to modify the filename
    std::string ModifyFilename(const std::string &filename,
                               const std::string &new_extension)
    {
        size_t last_dot = filename.find_last_of('.');
        size_t last_slash = filename.find_last_of("/\\");

        if (last_dot == std::string::npos ||
                (last_slash != std::string::npos && last_slash > last_dot))
        {
            // No extension found or the last dot is part of a directory
            return filename + new_extension;
        }
        else
        {
            return filename.substr(0, last_dot) + new_extension;
        }
    }

    // The main function to write the graph to a file
    void WriteGraph(std::string filename, bool serialized = false,
                    bool mtxed = false, bool edgelisted = false,
                    bool ligraed = false, bool structured = false)
    {
        if (filename == "")
        {
            std::cout << "No output filename given (Use -h for help)" << std::endl;
            std::exit(-8);
        }

        std::string original_filename = filename;
        if (serialized)
        {
            if (g_.is_weighted())
            {
                filename = ModifyFilename(original_filename, ".wsg");
            }
            else
            {
                filename = ModifyFilename(original_filename, ".sg");
            }
            std::fstream file(filename, std::ios::out | std::ios::binary);
            if (!file)
            {
                std::cout << "Couldn't write to file " << filename << std::endl;
                std::exit(-5);
            }
            std::cout << "writing to file " << filename << std::endl;
            WriteSerializedGraph(file);
            file.close();
        }

        if (edgelisted)
        {
            if (g_.is_weighted())
            {
                filename = ModifyFilename(original_filename, ".wel");
            }
            else
            {
                filename = ModifyFilename(original_filename, ".el");
            }
            std::fstream file(filename, std::ios::out | std::ios::binary);
            if (!file)
            {
                std::cout << "Couldn't write to file " << filename << std::endl;
                std::exit(-5);
            }
            std::cout << "writing to file " << filename << std::endl;
            WriteEL(file);
            file.close();
        }

        if (structured)
        {
            WriteGraphStructures(original_filename);
        }

        if (mtxed)
        {
            filename = ModifyFilename(original_filename, ".mtx");
            std::fstream file(filename, std::ios::out | std::ios::binary);
            if (!file)
            {
                std::cout << "Couldn't write to file " << filename << std::endl;
                std::exit(-5);
            }
            std::cout << "writing to file " << filename << std::endl;
            WriteMTX(file);
            file.close();
        }

        if (ligraed)
        {
            if (g_.is_weighted())
            {
                filename = ModifyFilename(original_filename, ".wligra");
            }
            else
            {
                filename = ModifyFilename(original_filename, ".ligra");
            }
            std::fstream file(filename, std::ios::out | std::ios::binary);
            if (!file)
            {
                std::cout << "Couldn't write to file " << filename << std::endl;
                std::exit(-5);
            }
            std::cout << "writing to file " << filename << std::endl;
            WriteLIGRA(file);
            file.close();
        }
    }

    void WriteLabels(std::string filename, bool serialized = false,
                     bool edgelisted = false)
    {
        if (filename == "")
        {
            std::cout << "No output filename given (Use -h for help)" << std::endl;
            std::exit(-8);
        }

        std::string original_filename = filename;
        if (serialized)
        {
            filename = ModifyFilename(original_filename, ".so");
            std::fstream file(filename, std::ios::out | std::ios::binary);
            if (!file)
            {
                std::cout << "Couldn't write to file " << filename << std::endl;
                std::exit(-5);
            }
            std::cout << "writing to file " << filename << std::endl;
            WriteSerializedLabels(file);
            file.close();
        }

        if (edgelisted)
        {
            filename = ModifyFilename(original_filename, ".lo");
            std::fstream file(filename, std::ios::out | std::ios::binary);
            if (!file)
            {
                std::cout << "Couldn't write to file " << filename << std::endl;
                std::exit(-5);
            }
            std::cout << "writing to file " << filename << std::endl;
            WriteListLabels(file);
            file.close();
        }
    }

private:
    CSRGraph<NodeID_, DestID_> &g_;
    std::string filename_;
};

#endif // WRITER_H_
