# Related Work

This page separates direct baselines from conceptual predecessors and broader
context. Links point to DOI records or official project pages.

## Direct lineage and graph-specific baselines

| Work | Mechanism | Relationship to ECG Next |
|---|---|---|
| Mughrabi, Baradaran, Samara, and Skadron, **“ECG: Expressing Locality and Prefetching for Optimal Caching in Graph Structures,”** IPDPSW 2024, pp. 520–525. [DOI](https://doi.org/10.1109/IPDPSW63119.2024.00105) | Packs graph-derived locality and prefetch information into graph records for cache and prefetch decisions. | Direct predecessor. ReusePlan replaces the single mask with a line tier and two future epochs, then binds them to the exact property request. |
| Faldu, Diamond, and Grot, **“Domain-Specialized Cache Management for Graph Analytics,”** HPCA 2020. [DOI](https://doi.org/10.1109/HPCA47549.2020.00028) · [artifact](https://github.com/faldupriyank/grasp) | GRASP uses software-identified graph-property regions and hot/moderate/cold insertion priorities. | Direct baseline and source of ReusePlan's tier-based insertion behavior. |
| Balaji, Crago, Jaleel, and Lucia, **“P-OPT: Practical Optimal Cache Replacement for Graph Analytics,”** HPCA 2021. [DOI](https://doi.org/10.1109/HPCA51647.2021.00062) · [artifact](https://github.com/CMUAbstract/POPT-CacheSim-HPCA21) | A graph-derived rereference matrix approximates farthest-future replacement. | Direct future-use baseline. ReusePlan attempts to replace the matrix with compact edge-carried epochs. |
| Basak et al., **“Analysis and Optimization of the Memory Hierarchy for Graph Processing Workloads,”** HPCA 2019. [DOI](https://doi.org/10.1109/HPCA.2019.00051) | DROPLET separates structure and property streams and uses edge data to prefetch indirect property accesses. | Direct prefetch baseline. DROPLET predicts what to fetch; ReusePlan describes how long to retain the fetched property line. |
| Manocha, Aragón, and Martonosi, **“Graphfire: Synergizing Fetch, Insertion, and Replacement Policies for Graph Analytics,”** IEEE TC 2023. [DOI](https://doi.org/10.1109/TC.2022.3157525) | Coordinates hardware-learned fetch, insertion, and replacement behavior for graph data. | Closest holistic graph-memory comparator; Graphfire learns online, while ReusePlan transports explicit graph-derived metadata. |
| Sharma et al., **“Data-Aware Cache Management for Graph Analytics,”** DATE 2022. [DOI](https://doi.org/10.23919/DATE54114.2022.9774709) | GRACE manages graph data types differently and bypasses data that does not benefit from caching. | Direct bypass/admission precedent. FlowThrough is narrower: it suppresses LLC insertion only for eligible record misses. |

## General replacement and admission foundations

| Work | Mechanism | Relationship to ECG Next |
|---|---|---|
| Qureshi et al., **“Adaptive Insertion Policies for High Performance Caching,”** ISCA 2007. [DOI](https://doi.org/10.1145/1250662.1250709) | BIP resists scans; DIP uses leader sets and set dueling to choose insertion policy. | Foundation for scan-resistant insertion and ReusePlan's online leader/follower control. |
| Jaleel et al., **“High Performance Cache Replacement Using Re-Reference Interval Prediction,”** ISCA 2010. [DOI](https://doi.org/10.1145/1815961.1815971) | RRIP stores a small predicted rereference interval per line and evicts maximum-RRPV lines. | Direct policy substrate for GRASP and the default ReusePlan victim rule. |
| Wu et al., **“SHiP: Signature-Based Hit Predictor for High Performance Caching,”** MICRO 2011. [DOI](https://doi.org/10.1145/2155620.2155671) | Signature-indexed counters predict whether a fill will be reused and choose RRIP insertion priority. | Conceptual predecessor for semantic insertion hints; ReusePlan uses graph-record metadata instead of PC correlation. |
| Jain and Lin, **“Back to the Future: Leveraging Belady’s Algorithm for Improved Cache Replacement,”** ISCA 2016. [DOI](https://doi.org/10.1109/ISCA.2016.17) | Hawkeye reconstructs sampled Belady decisions and predicts cache-friendly PCs. | Both use future-use structure; Hawkeye learns from execution, while ReusePlan delivers graph-computed epochs. |
| Faldu and Grot, **“Leeway: Addressing Variability in Dead-Block Prediction for Last-Level Caches,”** PACT 2017. [DOI](https://doi.org/10.1109/PACT.2017.32) · [artifact](https://github.com/faldupriyank/leeway) | Learns a signature's live distance and declares a line dead after its age exceeds that distance. | Generic lifetime-prediction predecessor to explicit future-epoch metadata. |
| Shah, Jain, and Lin, **“Effective Mimicry of Belady’s MIN Policy,”** HPCA 2022. [DOI](https://doi.org/10.1109/HPCA53966.2022.00048) | Mockingjay predicts ranked time-to-reuse and evicts the largest estimate. | Closest generic analogue to ReusePlan's ranked future distance. |

## Graph locality and layout context

| Work | Mechanism | Relationship to ECG Next |
|---|---|---|
| Zhang et al., **“Making Caches Work for Graph Analytics,”** IEEE Big Data 2017. [DOI](https://doi.org/10.1109/BigData.2017.8257937) | Cagra segments CSR and partitions work so random property accesses stay within an LLC-sized region. | Layout/execution alternative that manufactures locality rather than transporting replacement metadata. |
| Ham et al., **“Graphicionado: A High-Performance and Energy-Efficient Accelerator for Graph Analytics,”** MICRO 2016. [DOI](https://doi.org/10.1109/MICRO.2016.7783759) | A graph-specific pipeline and on-chip memory specialize edge streaming and vertex-state access. | Broader accelerator context; ECG Next targets a conventional cache hierarchy instead of a dedicated graph accelerator. |
| Faldu, Diamond, and Grot, **“A Closer Look at Lightweight Graph Reordering,”** IISWC 2019. [DOI](https://doi.org/10.1109/IISWC47752.2019.9041948) · [artifact](https://github.com/faldupriyank/dbg) | Degree-Based Grouping places coarse degree classes contiguously while preserving order within each class. | Direct source of the degree classes used by GRASP-style tiers. |

## Software-to-hardware cache guidance

| Work | Mechanism | Relationship to ECG Next |
|---|---|---|
| Wang, McKinley, Rosenberg, and Weems, **“Using the Compiler to Improve Cache Replacement Decisions,”** PACT 2002. [DOI](https://doi.org/10.1109/PACT.2002.1106018) | Compiler analysis marks low-value or last accesses and communicates replacement hints to hardware. | General precedent for ISA-visible cache guidance; ReuseBind attaches graph-specific metadata to the exact dynamic property load. |

## Position of ECG Next

ECG Next combines four ideas that prior work usually treats separately:

1. graph-derived tier and future-use analysis;
2. compact metadata in the graph record stream;
3. exact request-bound delivery to the consuming property load; and
4. record placement control that is independent of property replacement.

The direct evaluation baselines are GRASP, P-OPT, and DROPLET. Graphfire and
GRACE are the closest design comparators but are not implemented comparison
rows in the current evaluation. RRIP, SHiP, Hawkeye, Leeway, and Mockingjay
provide the general cache-policy context.
