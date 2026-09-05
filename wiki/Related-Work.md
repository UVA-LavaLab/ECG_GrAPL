# Related Work

The useful distinction is where reuse knowledge comes from and how it reaches
the cache: coarse software regions, a graph-derived matrix, a learned access
predictor, or metadata carried by the edge being consumed. This page relates
those mechanisms to current REF32, rather than treating every earlier ECG
variant as the same design.

## Direct lineage and graph-specific baselines

| Work | Mechanism | Relationship to current ECG |
|---|---|---|
| Mughrabi, Baradaran, Samara, and Skadron, **“ECG: Expressing Locality and Prefetching for Optimal Caching in Graph Structures,”** IPDPSW 2024, pp. 520–525. [DOI](https://doi.org/10.1109/IPDPSW63119.2024.00105) | Packs graph-derived locality and prefetch information into graph records for cache and prefetch decisions. | Direct lineage for edge-carried masks. Current REF32 uses Full14/Scale6 request-distance encodings and retirement-safe native association; distinguish this implementation from the published predecessor and intermediate two-epoch variants. |
| Faldu, Diamond, and Grot, **“Domain-Specialized Cache Management for Graph Analytics,”** HPCA 2020. [DOI](https://doi.org/10.1109/HPCA47549.2020.00028) · [artifact](https://github.com/faldupriyank/grasp) | GRASP uses software-identified graph-property regions and hot/moderate/cold insertion priorities. | Direct baseline and local fallback context. REF32's local tier is derived at the cache, not another field hidden in the six-bit token. The paper-faithful baseline and older sensitivity remain distinct. |
| Balaji, Crago, Jaleel, and Lucia, **“P-OPT: Practical Optimal Cache Replacement for Graph Analytics,”** HPCA 2021. [DOI](https://doi.org/10.1109/HPCA51647.2021.00062) · [artifact](https://github.com/CMUAbstract/POPT-CacheSim-HPCA21) | A graph-derived rereference matrix approximates farthest-future replacement. | Direct future-use baseline. REF32 carries a bounded next-property-line reference with the edge instead of consulting a runtime matrix. Active-column and backing-store costs scale with the graph. |
| Basak et al., **“Analysis and Optimization of the Memory Hierarchy for Graph Processing Workloads,”** HPCA 2019. [DOI](https://doi.org/10.1109/HPCA.2019.00051) | DROPLET separates structure and property streams and uses edge data to prefetch indirect property accesses. | Prefetch-design comparator: the future record identifies the target property. REF32 separately represents reuse lifetime; its current Twitter roster does not contain a DROPLET row. |
| Manocha, Aragón, and Martonosi, **“Graphfire: Synergizing Fetch, Insertion, and Replacement Policies for Graph Analytics,”** IEEE TC 2023. [DOI](https://doi.org/10.1109/TC.2022.3157525) | Coordinates hardware-learned fetch, insertion, and replacement behavior for graph data. | Related comparator: online access learning differs from transporting graph/traversal-derived information with each edge. It is not a current comparison row. |
| Sharma et al., **“Data-Aware Cache Management for Graph Analytics,”** DATE 2022. [DOI](https://doi.org/10.23919/DATE54114.2022.9774709) | GRACE manages graph data types differently and bypasses data that does not benefit from caching. | Admission/bypass context. REF32's functional known-dead bypass and optional structural FlowThrough are distinct mechanisms; native speculative known-dead bypass is not implemented. |

P-OPT-SE deserves its own fidelity boundary. The paper describes one-column
residency, but the pinned artifact does not implement that variant. The
repository therefore keeps two disclosed reconstructions of its unspecified
post-final-use case rather than presenting either as an exact reproduction.
See [baseline accounting](Evaluation-Methodology#52-single-epoch-p-opt-reconstruction).

## General replacement and admission foundations

| Work | Mechanism | Relationship to current ECG |
|---|---|---|
| Qureshi et al., **“Adaptive Insertion Policies for High Performance Caching,”** ISCA 2007. [DOI](https://doi.org/10.1145/1250662.1250709) | BIP resists scans; DIP uses leader sets and set dueling to choose insertion policy. | Scan-resistant insertion and adaptive-policy context; current REF32 is not the earlier leader/follower ReusePlan controller. |
| Jaleel et al., **“High Performance Cache Replacement Using Re-Reference Interval Prediction,”** ISCA 2010. [DOI](https://doi.org/10.1145/1815961.1815971) | RRIP stores a small predicted rereference interval per line and evicts maximum-RRPV lines. | REF32 maps finite distance bounds to an RRPV-like score and retains RRIP/GRASP fallback for unknown predictions. This is not the old RRIP-first two-epoch rule. |
| Wu et al., **“SHiP: Signature-Based Hit Predictor for High Performance Caching,”** MICRO 2011. [DOI](https://doi.org/10.1145/2155620.2155671) | Signature-indexed counters predict whether a fill will be reused and choose RRIP insertion priority. | Learned signatures provide different information from an edge's traversal-derived next-line reference. |
| Jain and Lin, **“Back to the Future: Leveraging Belady’s Algorithm for Improved Cache Replacement,”** ISCA 2016. [DOI](https://doi.org/10.1109/ISCA.2016.17) | Hawkeye reconstructs sampled Belady decisions and predicts cache-friendly PCs. | Both exploit future-use structure; Hawkeye learns from execution, whereas REF32 carries a graph-derived reference. |
| Faldu and Grot, **“Leeway: Addressing Variability in Dead-Block Prediction for Last-Level Caches,”** PACT 2017. [DOI](https://doi.org/10.1109/PACT.2017.32) · [artifact](https://github.com/faldupriyank/leeway) | Learns a signature's live distance and predicts when a block is dead. | Lifetime-prediction context. An expired REF32 bound becomes UNKNOWN; it is not sufficient evidence of DEAD. |
| Shah, Jain, and Lin, **“Effective Mimicry of Belady’s MIN Policy,”** HPCA 2022. [DOI](https://doi.org/10.1109/HPCA53966.2022.00048) | Mockingjay predicts ranked time-to-reuse and selects a victim using that prediction. | A ranked-future analogue with a different source of knowledge. REF32's position is in governed-request units, not CPU cycles. |

## Graph locality and layout context

| Work | Mechanism | Relationship to current ECG |
|---|---|---|
| Zhang et al., **“Making Caches Work for Graph Analytics,”** IEEE Big Data 2017. [DOI](https://doi.org/10.1109/BigData.2017.8257937) | Cagra segments CSR and partitions work so random property accesses stay within an LLC-sized region. | Layout/execution changes manufacture locality; ECG transports reuse information about the traversal it actually executes. |
| Ham et al., **“Graphicionado: A High-Performance and Energy-Efficient Accelerator for Graph Analytics,”** MICRO 2016. [DOI](https://doi.org/10.1109/MICRO.2016.7783759) | A graph-specific pipeline and on-chip memory specialize edge streaming and vertex-state access. | Architectural context for connecting graph arrays to a pipeline. The current native ECG path extends a conventional core/cache hierarchy rather than claiming a complete dedicated accelerator. |
| Faldu, Diamond, and Grot, **“A Closer Look at Lightweight Graph Reordering,”** IISWC 2019. [DOI](https://doi.org/10.1109/IISWC47752.2019.9041948) · [artifact](https://github.com/faldupriyank/dbg) | Degree-Based Grouping places coarse degree classes contiguously while preserving order within each class. | Layout affects cache-line sharing and request order. REF32 must be constructed for the reordered traversal, not copied from a different order. |

## Software-to-hardware guidance

Wang, McKinley, Rosenberg, and Weems,
**“Using the Compiler to Improve Cache Replacement Decisions,”** PACT 2002
([DOI](https://doi.org/10.1109/PACT.2002.1106018)), is a precedent for software
analysis communicating low-value or last accesses to hardware. Here the
source of knowledge is the graph plus traversal, and the native path preserves
the hint's association with the specific load through rename and retirement.

## Position of the current implementation

The mechanism has four connected parts: derive property-line reuse from a
known traversal; choose an encoding that fits the ID headroom; recover the
ordinary address/value while retaining the hint's dynamic association; and
use bounded cache-side state for replacement or prefetch decisions.

The completed Twitter comparisons include LRU, SRRIP, `GRASP_PAPER`, full
P-OPT controls and the two SE reconstructions. Full14 is the richer small-ID
encoding; Scale6 is the compact large-ID choice. Neither an encoding choice
nor an earlier backend implementation establishes native prefetch, production
timing or physical-area results that have not been completed.
