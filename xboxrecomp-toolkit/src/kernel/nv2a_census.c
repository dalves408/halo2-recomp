/* GPU feature census; see nv2a_census.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nv2a_census.h"

#define CENSUS_SLOTS 8192u            /* power of two */

typedef struct {
    const char *category;
    uint32_t key;
    uint64_t count, vertices;
} CensusEntry;

static CensusEntry s_entries[CENSUS_SLOTS];
static uint32_t s_used, s_dropped;
static int s_enabled = -1;
static int s_gate_open;
static const char *s_path, *s_gate;
static uint32_t s_flips_counted;

static void census_init(void)
{
    if (s_enabled >= 0)
        return;
    s_path = getenv("RECOMP_GPU_CENSUS");
    s_gate = getenv("RECOMP_GPU_CENSUS_GATE");
    if (s_gate && !*s_gate)
        s_gate = NULL;
    s_enabled = s_path && *s_path;
    s_gate_open = s_enabled && !s_gate;
    if (s_enabled)
        fprintf(stderr, "[GPU] feature census -> %s%s%s\n", s_path,
                s_gate ? " while this file exists: " : "", s_gate ? s_gate : "");
}

int nv2a_census_active(void)
{
    census_init();
    return s_gate_open;
}

void nv2a_census_flip(void)
{
    census_init();
    if (!s_enabled)
        return;
    if (s_gate) {
        FILE *f = fopen(s_gate, "rb");
        s_gate_open = f != NULL;
        if (f)
            fclose(f);
    }
    if (s_gate_open)
        s_flips_counted++;
}

void nv2a_census_add(const char *category, uint32_t key, uint32_t vertices)
{
    uint64_t h;
    uint32_t i, n;

    if (!nv2a_census_active())
        return;
    h = ((uint64_t)(uintptr_t)category * 0x9E3779B97F4A7C15ull) ^ (key * 0xC2B2AE3D27D4EB4Full);
    i = (uint32_t)(h >> 40) & (CENSUS_SLOTS - 1);
    for (n = 0; n < CENSUS_SLOTS; n++, i = (i + 1) & (CENSUS_SLOTS - 1)) {
        CensusEntry *e = &s_entries[i];
        if (!e->category) {
            if (s_used >= CENSUS_SLOTS * 3 / 4) {
                s_dropped++;
                return;
            }
            e->category = category;
            e->key = key;
            s_used++;
        } else if (e->category != category || e->key != key) {
            continue;
        }
        e->count++;
        e->vertices += vertices;
        return;
    }
    s_dropped++;
}

static int entry_order(const void *pa, const void *pb)
{
    const CensusEntry *a = (const CensusEntry *)pa, *b = (const CensusEntry *)pb;
    int c;

    if (!a->category || !b->category)
        return !a->category - !b->category;
    c = strcmp(a->category, b->category);
    if (c)
        return c;
    if (a->count != b->count)
        return a->count < b->count ? 1 : -1;
    return a->key < b->key ? -1 : a->key > b->key;
}

void nv2a_census_write(void)
{
    static CensusEntry sorted[CENSUS_SLOTS];
    char temp[1024];
    FILE *f;
    uint32_t i;

    census_init();
    if (!s_enabled)
        return;
    memcpy(sorted, s_entries, sizeof sorted);
    qsort(sorted, CENSUS_SLOTS, sizeof sorted[0], entry_order);
    snprintf(temp, sizeof temp, "%s.tmp", s_path);
    f = fopen(temp, "w");
    if (!f)
        return;
    fprintf(f, "# GPU feature census: %u flips counted, %u keys, %u dropped\n",
            s_flips_counted, s_used, s_dropped);
    fprintf(f, "category,key,count,vertices\n");
    for (i = 0; i < CENSUS_SLOTS && sorted[i].category; i++)
        fprintf(f, "%s,%08X,%llu,%llu\n", sorted[i].category, sorted[i].key,
                (unsigned long long)sorted[i].count,
                (unsigned long long)sorted[i].vertices);
    if (fclose(f) != 0)
        return;
    remove(s_path);
    rename(temp, s_path);
}

void nv2a_census_reset(void)
{
    memset(s_entries, 0, sizeof s_entries);
    s_used = s_dropped = s_flips_counted = 0;
    s_enabled = -1;
    s_gate_open = 0;
}
