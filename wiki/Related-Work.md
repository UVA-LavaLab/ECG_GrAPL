# Related Work

The useful distinction is where reuse knowledge comes from and how it reaches
the cache: coarse software regions, a graph-derived matrix, a learned access
predictor, or metadata carried by the edge being consumed. This page relates
those mechanisms to the current adaptive ECG method, rather than treating every earlier ECG
variant as the same design.

## Direct lineage and graph-specific baselines

| Work | Mechanism | Relationship to current ECG |
|---|---|---|
| Mughrabi, Baradaran, Samara, and Skadron, **“ECG: Expressing Locality and Prefetching for Optimal Caching in Graph Structures,”** IPDPSW 2024, pp. 520–525. [DOI](https://doi.org/10.1109/IPDPSW63119.2024.00105) | Packs graph-derived locality and prefetch information into graph records for cache and prefetch decisions. | Direct lineage for edge-carried metadata. The current implementation uses one graph-derived joint state/distance grammar, real record-window prefetching, and retirement-safe native association; it remains distinct from the published predecessor and intermediate variants. |
| Faldu, Diamond, and Grot, **“Domain-Specialized Cache Management for Graph Analytics,”** HPCA 2020. [DOI](https://doi.org/10.1109/HPCA47549.2020.00028) · [artifact](https://github.com/faldupriyank/grasp) | GRASP uses software-identified graph-property regions and hot/moderate/cold insertion priorities. | Direct baseline and local fallback context. ECG derives local tier at the cache rather than storing another record field. The paper-faithful baseline and older sensitivity remain distinct. |
| Balaji, Crago, Jaleel, and Lucia, **“P-OPT: Practical Optimal Cache Replacement for Graph Analytics,”** HPCA 2021. [DOI](https://doi.org/10.1109/HPCA51647.2021.00062) · [artifact](https://github.com/CMUAbstract/POPT-CacheSim-HPCA21) | A graph-derived rereference matrix approximates farthest-future replacement. | Direct future-use baseline. ECG carries a bounded next-property-line reference with the edge instead of consulting a runtime matrix. Active-column and backing-store costs scale with the graph. |
| Basak et al., **“Analysis and Optimization of the Memory Hierarchy for Graph Processing Workloads,”** HPCA 2019. [DOI](https://doi.org/10.1109/HPCA.2019.00051) | DROPLET separates structure and property streams and uses edge data to prefetch indirect property accesses. | Prefetch-design comparator: a future ECG record identifies the target property, while the selected record's joint token separately describes reuse lifetime. |
| Manocha, Aragón, and Martonosi, **“Graphfire: Synergizing Fetch, Insertion, and Replacement Policies for Graph Analytics,”** IEEE TC 2023. [DOI](https://doi.org/10.1109/TC.2022.3157525) | Coordinates hardware-learned fetch, insertion, and replacement behavior for graph data. | Related comparator: online access learning differs from transporting graph/traversal-derived information with each edge. It is not a current comparison row. |
| Sharma et al., **“Data-Aware Cache Management for Graph Analytics,”** DATE 2022. [DOI](https://doi.org/10.23919/DATE54114.2022.9774709) | GRACE manages graph data types differently and bypasses data that does not benefit from caching. | Admission/bypass context. ECG's request-scoped known-DEAD miss bypass and historical structural FlowThrough are distinct mechanisms. |

P-OPT-SE deserves its own fidelity boundary. The paper describes one-column
residency, but the pinned artifact does not implement that variant. The
repository therefore keeps two disclosed reconstructions of its unspecified
post-final-use case rather than presenting either as an exact reproduction.
See [baseline accounting](Evaluation-Methodology#52-single-epoch-p-opt-reconstruction).

## General replacement and admission foundations

| Work | Mechanism | Relationship to current ECG |
|---|---|---|
| Qureshi et al., **“Adaptive Insertion Policies for High Performance Caching,”** ISCA 2007. [DOI](https://doi.org/10.1145/1250662.1250709) | BIP resists scans; DIP uses leader sets and set dueling to choose insertion policy. | Scan-resistant insertion and adaptive-policy context; current ECG is not the earlier leader/follower ReusePlan controller. |
| Jaleel et al., **“High Performance Cache Replacement Using Re-Reference Interval Prediction,”** ISCA 2010. [DOI](https://doi.org/10.1145/1815961.1815971) | RRIP stores a small predicted rereference interval per line and evicts maximum-RRPV lines. | ECG maps finite distance bounds to an RRPV-like victim score while prediction updates leave ordinary RRPV untouched. UNKNOWN uses the local RRIP/GRASP fallback. |
| Wu et al., **“SHiP: Signature-Based Hit Predictor for High Performance Caching,”** MICRO 2011. [DOI](https://doi.org/10.1145/2155620.2155671) | Signature-indexed counters predict whether a fill will be reused and choose RRIP insertion priority. | Learned signatures provide different information from an edge's traversal-derived next-line reference. |
| Jain and Lin, **“Back to the Future: Leveraging Belady’s Algorithm for Improved Cache Replacement,”** ISCA 2016. [DOI](https://doi.org/10.1109/ISCA.2016.17) | Hawkeye reconstructs sampled Belady decisions and predicts cache-friendly PCs. | Both exploit future-use structure; Hawkeye learns from execution, whereas ECG carries a graph-derived reference. |
| Faldu and Grot, **“Leeway: Addressing Variability in Dead-Block Prediction for Last-Level Caches,”** PACT 2017. [DOI](https://doi.org/10.1109/PACT.2017.32) · [artifact](https://github.com/faldupriyank/leeway) | Learns a signature's live distance and predicts when a block is dead. | Lifetime-prediction context. An expired ECG bound becomes UNKNOWN; it is not sufficient evidence of DEAD. |
| Shah, Jain, and Lin, **“Effective Mimicry of Belady’s MIN Policy,”** HPCA 2022. [DOI](https://doi.org/10.1109/HPCA53966.2022.00048) | Mockingjay predicts ranked time-to-reuse and selects a victim using that prediction. | A ranked-future analogue with a different source of knowledge. ECG's position is in governed-request units, not CPU cycles. |

## Graph locality and layout context

| Work | Mechanism | Relationship to current ECG |
|---|---|---|
| Zhang et al., **“Making Caches Work for Graph Analytics,”** IEEE Big Data 2017. [DOI](https://doi.org/10.1109/BigData.2017.8257937) | Cagra segments CSR and partitions work so random property accesses stay within an LLC-sized region. | Layout/execution changes manufacture locality; ECG transports reuse information about the traversal it actually executes. |
| Ham et al., **“Graphicionado: A High-Performance and Energy-Efficient Accelerator for Graph Analytics,”** MICRO 2016. [DOI](https://doi.org/10.1109/MICRO.2016.7783759) | A graph-specific pipeline and on-chip memory specialize edge streaming and vertex-state access. | Architectural context for connecting graph arrays to a pipeline. The current native ECG path extends a conventional core/cache hierarchy rather than claiming a complete dedicated accelerator. |
| Faldu, Diamond, and Grot, **“A Closer Look at Lightweight Graph Reordering,”** IISWC 2019. [DOI](https://doi.org/10.1109/IISWC47752.2019.9041948) · [artifact](https://github.com/faldupriyank/dbg) | Degree-Based Grouping places coarse degree classes contiguously while preserving order within each class. | Layout affects cache-line sharing and request order. ECG records must be constructed for the reordered traversal, not copied from a different order. |

## Software-to-hardware guidance

Wang, McKinley, Rosenberg, and Weems,
**“Using the Compiler to Improve Cache Replacement Decisions,”** PACT 2002
([DOI](https://doi.org/10.1109/PACT.2002.1106018)), is a precedent for software
analysis communicating low-value or last accesses to hardware. Here the
source of knowledge is the graph plus traversal, and the native path preserves
the hint's association with the specific load through rename and retirement.

Vijaykumar et al., **"A Case for Richer Cross-Layer Abstractions: Bridging the
Semantic Gap with Expressive Memory,"** ISCA 2018
([DOI](https://doi.org/10.1109/ISCA.2018.00027)), supplies broader precedent
for communicating software semantics to the memory system. A cross-layer
hint interface alone is therefore not the contribution claimed here.

## Position of the current implementation

The mechanism has four connected parts: derive property-line reuse from a
known traversal; choose an encoding that fits the ID headroom; recover the
ordinary address/value while retaining the hint's dynamic association; and
use bounded cache-side state for replacement or prefetch decisions.

The current method chooses widths bit by bit from the actual graph and uses one
joint state/distance grammar and one real-record window rule. Functional and
native mechanisms are implemented; Sniper has strict current-path admission
but remains modeled corroboration. Historical Twitter comparisons preserve
their original fixed-format labels and P-OPT controls. Neither those results
nor current logical bit counts establish final campaign or silicon-area claims.

### Full-paper successor to ECG 2024

The authors' **ECG 2024 workshop paper** is the published foundation.
This work is its full-paper successor: an improved graph-adaptive reuse model
and a complete native RISC-V/gem5 realization of the specified mechanism,
with functional and Sniper corroboration. The contribution is the technical
advance over the workshop work, not a claim that every underlying ingredient
is independently new.

The closest published overlap is concrete:

| Published source | Already established | Full-paper extension or comparison |
|---|---|---|
| [ECG 2024, pp. 520-521, Sections III.A-B](https://www.cs.virginia.edu/~rgq5aw/files/ecg.pdf) | Spare vertex-ID bits can carry GRASP/P-OPT/prefetch information, consumed through a specialized graph-addressing path. | The successor defines line-next-use semantics and one graph-derived joint state/distance code, with actual raw-record/property association, retirement-safe resident updates, and bounded fetched-window prefetching. |
| [P-OPT, Sections III-V](https://users.ece.cmu.edu/~vigneshb/papers/POPT_HPCA21_CameraReady.pdf) | Graph-transpose-derived future references, epoch quantization, a rereference matrix, and current/next columns in reserved LLC ways. | Per-record reference position and delivery through the consumed edge replace a separate runtime matrix; matched-capacity and costed controls must separate prediction quality from capacity and traffic advantages. |
| [GRASP, HPCA 2020](https://ease-lab.github.io/ease_website/pubs/GRASP_HPCA20.pdf) | Lightweight graph/software guidance and preferential cache treatment for hot vertices. | Current next-line-reference annotations are not merely another hot/cold region hint. |
| [Basak et al., HPCA 2019 / DROPLET](https://doi.org/10.1109/HPCA.2019.00051) ([author slides](https://abasak24.github.io/slides/hpca2019_droplet.pdf)) | Fetched graph structure can drive decoupled indirect-property prefetching. | The reuse-ranked bounded window, actual-byte readiness, LLC-only allocation and accounting must explain a meaningful difference beyond structure-triggered prefetching alone. |

The contribution of the successor is:

> Building on the ECG 2024 workshop paper, this full-paper successor develops
> an improved graph-adaptive property-line reuse model and its native
> RISC-V/gem5 implementation. It carries each consumed edge's annotation
> through the actual dependent property load, applies resident prediction
> updates after retirement without refreshing ordinary recency, and reuses
> the fetched record stream for bounded, reuse-guided LLC lookahead.

The reviewed peer-reviewed sources did not establish this exact combination
as a single mechanism. That is a bounded literature finding, **not** a
guarantee that no related work exists. Offline next-use prediction,
quantization, software hints, resident reuse metadata and structure-driven
prefetching are not individually new.

The paper must connect the distinction to evidence: comparison with the
published ECG predecessor, replacement/prefetch ablations, equal-data-capacity
P-OPT controls, approximation/record-width sensitivity, and full hardware
state/port/latency costs. The archived Twitter controls and the current
reproduction are relevant to attribution, but historical results must not be
relabeled as the current implementation. See the
[hardware evidence boundary](RISC-V-Instruction-Path#5-state-and-evidence-boundaries)
before describing the design as low-overhead hardware.
