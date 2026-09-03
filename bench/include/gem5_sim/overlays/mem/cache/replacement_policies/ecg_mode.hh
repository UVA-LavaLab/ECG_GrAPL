#ifndef GRAPHBREW_ECG_MODE_H
#define GRAPHBREW_ECG_MODE_H

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace ecg_mode {

// Values are explicit so a future serialized representation cannot drift.
enum class Mode : uint8_t {
    DBG_PRIMARY = 0,
    POPT_PRIMARY = 1,
    POPT_TIE = 2,
    DBG_ONLY = 3,
    ECG_EMBEDDED = 4,
    ECG_EPOCH_EMBEDDED = 5,
    ECG_COMBINED = 6,
    ECG_EXACT = 7,
    ECG_EXACT_STORED = 8,
    ECG_EXACT_MASK = 9,
    ECG_GRASP_POPT = 10,
    ECG_REF32 = 11,
};

inline const char* name(Mode mode) {
    switch (mode) {
        case Mode::DBG_PRIMARY: return "DBG_PRIMARY";
        case Mode::POPT_PRIMARY: return "POPT_PRIMARY";
        case Mode::POPT_TIE: return "POPT_TIE";
        case Mode::DBG_ONLY: return "DBG_ONLY";
        case Mode::ECG_EMBEDDED: return "ECG_EMBEDDED";
        case Mode::ECG_EPOCH_EMBEDDED: return "ECG_EPOCH_EMBEDDED";
        case Mode::ECG_COMBINED: return "ECG_COMBINED";
        case Mode::ECG_EXACT: return "ECG_EXACT";
        case Mode::ECG_EXACT_STORED: return "ECG_EXACT_STORED";
        case Mode::ECG_EXACT_MASK: return "ECG_EXACT_MASK";
        case Mode::ECG_GRASP_POPT: return "ECG_GRASP_POPT";
        case Mode::ECG_REF32: return "ECG_REF32";
    }
    return "UNKNOWN";
}

inline Mode parse(const std::string& text) {
    if (text.empty() || text == "DBG_PRIMARY" || text == "dbg_primary")
        return Mode::DBG_PRIMARY;
    if (text == "POPT_PRIMARY" || text == "popt_primary" || text == "popt")
        return Mode::POPT_PRIMARY;
    if (text == "POPT_TIE" || text == "popt_tie" ||
        text == "popt_tiebreak")
        return Mode::POPT_TIE;
    if (text == "DBG_ONLY" || text == "dbg_only" || text == "dbg")
        return Mode::DBG_ONLY;
    if (text == "ECG_EMBEDDED" || text == "ecg_embedded" ||
        text == "embedded")
        return Mode::ECG_EMBEDDED;
    if (text == "ECG_EPOCH_EMBEDDED" ||
        text == "ecg_epoch_embedded" || text == "epoch_embedded")
        return Mode::ECG_EPOCH_EMBEDDED;
    if (text == "ECG_COMBINED" || text == "ecg_combined" ||
        text == "combined")
        return Mode::ECG_COMBINED;
    if (text == "ECG_EXACT" || text == "ecg_exact" || text == "exact")
        return Mode::ECG_EXACT;
    if (text == "ECG_EXACT_STORED" ||
        text == "ecg_exact_stored" || text == "exact_stored")
        return Mode::ECG_EXACT_STORED;
    if (text == "ECG_EXACT_MASK" || text == "ecg_exact_mask" ||
        text == "exact_mask")
        return Mode::ECG_EXACT_MASK;
    if (text == "ECG_GRASP_POPT" || text == "GRASP_POPT" ||
        text == "ecg_grasp_popt" || text == "grasp_popt")
        return Mode::ECG_GRASP_POPT;
    if (text == "ECG_REF32" || text == "ecg_ref32" || text == "ref32")
        return Mode::ECG_REF32;
    std::fprintf(
        stderr,
        "[FATAL] unknown ECG mode '%s'; expected DBG_PRIMARY, POPT_PRIMARY, "
        "POPT_TIE, DBG_ONLY, ECG_EMBEDDED, ECG_EPOCH_EMBEDDED, "
        "ECG_COMBINED, ECG_EXACT, ECG_EXACT_STORED, ECG_EXACT_MASK, or "
        "ECG_GRASP_POPT, or ECG_REF32\n",
        text.c_str());
    std::abort();
}

inline bool supportedByAllBackends(Mode mode) {
    return mode == Mode::DBG_PRIMARY ||
           mode == Mode::POPT_PRIMARY ||
           mode == Mode::DBG_ONLY ||
           mode == Mode::ECG_EMBEDDED ||
           mode == Mode::ECG_COMBINED ||
           mode == Mode::ECG_GRASP_POPT;
}

}  // namespace ecg_mode

#endif
