#include "ecg_epoch_builder.h"
#include "ecg_victim_policy.h"

#include <cstdint>
#include <cstdio>
#include <vector>

struct TinyGraph {
    std::vector<std::vector<int>> out;
    std::vector<std::vector<int>> in;

    explicit TinyGraph(uint32_t n) : out(n), in(n) {}

    void addEdge(int u, int v) {
        out[u].push_back(v);
        in[v].push_back(u);
    }

    uint32_t num_nodes() const { return static_cast<uint32_t>(out.size()); }
    uint64_t num_edges_directed() const {
        uint64_t total = 0;
        for (const auto& row : out) total += row.size();
        return total;
    }
    const std::vector<int>& out_neigh(uint32_t v) const { return out[v]; }
    const std::vector<int>& in_neigh(uint32_t v) const { return in[v]; }
};

static bool checkDirection(const TinyGraph& graph, bool push)
{
    std::vector<std::vector<uint16_t>> single;
    std::vector<std::vector<ecg_epoch::EpochPair>> pairs;
    std::vector<uint64_t> record_off;
    std::vector<uint64_t> records;
    ecg_epoch::buildInEdgeEpochs(
        graph, 2, 32, true, single, push);
    ecg_epoch::buildInEdgeEpochPairs(
        graph, 2, 32, true, pairs, push);
    ecg_epoch::buildInEdgeEpochPairRecords(
        graph, 2, 32, true, record_off, records, push);

    if (single.size() != pairs.size()) return false;
    for (size_t src = 0; src < single.size(); ++src) {
        if (single[src].size() != pairs[src].size()) return false;
        for (size_t edge = 0; edge < single[src].size(); ++edge) {
            if (!pairs[src][edge].valid) return false;
            if (pairs[src][edge].first != single[src][edge]) return false;
            const uint64_t record = records[record_off[src] + edge];
            if (ecg_epoch::extractEpochPairFirst(record) !=
                    pairs[src][edge].first ||
                ecg_epoch::extractEpochPairSecond(record) !=
                    pairs[src][edge].second ||
                ecg_epoch::extractEpochPairTier(record) !=
                    pairs[src][edge].tier)
                return false;
        }
    }
    if (push) {
        // Edge 1->2: property line 2's readers after src=1 are reader 5,
        // then reader 0 after wrap. NE=32,N=6 => epochs 26 then 0.
        if (pairs[1].empty() ||
            pairs[1][0].first != 26 ||
            pairs[1][0].second != 0)
            return false;
    }
    return true;
}

static bool checkSingleReaderWrap()
{
    TinyGraph graph(16);
    graph.addEdge(1, 0);
    graph.addEdge(4, 1);
    graph.addEdge(12, 2);

    std::vector<std::vector<ecg_epoch::EpochPair>> pairs;
    ecg_epoch::buildInEdgeEpochPairs(
        graph, 16, 16, true, pairs, true);
    return pairs.size() > 12 && pairs[12].size() == 1 &&
           pairs[12][0].first == 1 &&
           pairs[12][0].second == 4;
}

static bool checkReuseTiers()
{
    const uint64_t counts[] = {10, 1, 7, 0, 3, 2, 0, 0, 0, 0};
    std::vector<uint64_t> off(11, 0);
    for (size_t i = 0; i < 10; ++i) off[i + 1] = off[i] + counts[i];
    const auto tiers = ecg_epoch::buildReuseTiers(off, 10, 0.10);
    return tiers.size() == 10 &&
           tiers[0] == 1 &&
           tiers[2] == 2 &&
           tiers[1] == 3 &&
           tiers[4] == 3;
}

static bool checkK1FullRange(const TinyGraph& graph)
{
    std::vector<std::vector<uint16_t>> epochs;
    ecg_epoch::buildInEdgeEpochs(
        graph, 2, 65535, true, epochs, true);
    return epochs.size() > 1 && !epochs[1].empty() &&
           epochs[1][0] > ecg_epoch::kK2EpochMask;
}

int main()
{
    TinyGraph graph(6);
    graph.addEdge(0, 1);
    graph.addEdge(0, 2);
    graph.addEdge(1, 2);
    graph.addEdge(1, 3);
    graph.addEdge(2, 1);
    graph.addEdge(2, 4);
    graph.addEdge(3, 1);
    graph.addEdge(3, 5);
    graph.addEdge(4, 1);
    graph.addEdge(5, 2);

    const bool pull_ok = checkDirection(graph, false);
    const bool push_ok = checkDirection(graph, true);
    const bool single_reader_ok = checkSingleReaderWrap();
    const bool tiers_ok = checkReuseTiers();
    const bool k1_range_ok = checkK1FullRange(graph);
    const uint64_t record =
        ecg_epoch::packEpochPairRecord(0x89ABCDEFu, 2, 26, 0);
    const bool wire_ok =
        ecg_epoch::extractEpochPairDest(record) == 0x89ABCDEFu &&
        ecg_epoch::extractEpochPairTier(record) == 2 &&
        ecg_epoch::extractEpochPairFirst(record) == 26 &&
        ecg_epoch::extractEpochPairSecond(record) == 0;
    const bool distance_ok =
        ecg_policy::epochPairDistance(26, 0, 1, 27, 32) == 31 &&
        ecg_policy::epochPairDistance(26, 0, 2, 27, 32) == 5 &&
        ecg_policy::epochPairDistance(65, 130, 2, 0, 65535) == 65;
    std::printf(
        "[test_ecg_epoch_pair] pull=%s push=%s single-reader=%s tiers=%s "
        "k1-range=%s "
        "wire=%s distance=%s\n",
                pull_ok ? "OK" : "FAIL",
                push_ok ? "OK" : "FAIL",
                single_reader_ok ? "OK" : "FAIL",
                tiers_ok ? "OK" : "FAIL",
                k1_range_ok ? "OK" : "FAIL",
                wire_ok ? "OK" : "FAIL",
                distance_ok ? "OK" : "FAIL");
    return pull_ok && push_ok && single_reader_ok && tiers_ok && k1_range_ok &&
        wire_ok && distance_ok
        ? 0 : 1;
}
