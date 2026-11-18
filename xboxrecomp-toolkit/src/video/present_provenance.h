#ifndef XBOX_PRESENT_PROVENANCE_H
#define XBOX_PRESENT_PROVENANCE_H

#include <stdint.h>

enum RecompPresentStats {
    RECOMP_PRESENT_STATS_UNTRACKED,
    RECOMP_PRESENT_STATS_DISABLED,
    RECOMP_PRESENT_STATS_READY,
    RECOMP_PRESENT_STATS_NO_ID,
    RECOMP_PRESENT_STATS_EXHAUSTED
};

enum RecompPresentPixels {
    RECOMP_PRESENT_PIXELS_UNKNOWN,
    RECOMP_PRESENT_PIXELS_CPU,
    RECOMP_PRESENT_PIXELS_READBACK,
    RECOMP_PRESENT_PIXELS_READBACK_FAILED,
    RECOMP_PRESENT_PIXELS_READBACK_UNAVAILABLE
};

/* A candidate is not an alignment until the matching statistics row has
 * actually been written and independently checked. */
typedef struct RecompPresentTag {
    uint32_t flip_candidate;
    uint32_t stats_volume;
    uint64_t stats_file_index;
    enum RecompPresentStats stats;
    enum RecompPresentPixels pixels;
} RecompPresentTag;

static inline RecompPresentTag
recomp_present_tag(enum RecompPresentStats stats, uint32_t previous_flip,
                   uint32_t volume, uint64_t file_index,
                   enum RecompPresentPixels pixels)
{
    RecompPresentTag tag = {0, 0, 0, stats, pixels};

    if (stats == RECOMP_PRESENT_STATS_READY) {
        if (previous_flip == UINT32_MAX) {
            tag.stats = RECOMP_PRESENT_STATS_EXHAUSTED;
        } else {
            tag.stats_volume = volume;
            tag.stats_file_index = file_index;
            if (pixels == RECOMP_PRESENT_PIXELS_CPU ||
                pixels == RECOMP_PRESENT_PIXELS_READBACK)
                tag.flip_candidate = previous_flip + 1;
        }
    }
    return tag;
}

#endif
