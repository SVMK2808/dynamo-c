#ifndef DYNAMO_VECTOR_CLOCK_H
#define DYNAMO_VECTOR_CLOCK_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * An individual entry in a Vector Clock.
 * 
 * In Dynamo, each actor/node that performs a write increments its own counter
 * and records a wall-clock timestamp. The timestamp is used specifically for
 * clock truncation (pruning) when clocks grow too large.
 */
typedef struct {
    uint32_t node_id;    /* Identifier for the physical/logical node */
    uint64_t counter;    /* Monotonically increasing sequence number */
    uint64_t timestamp;  /* Epoch time (sec or ms) of last modification */
} vclock_entry_t;

/**
 * Dynamic array of vector clock entries, kept SORTED by node_id.
 * 
 * Maintaining sorted order by node_id is a critical design choice:
 * 1. Searching for an existing node is O(log N) via binary search.
 * 2. Comparing two vector clocks is O(N1 + N2) via a two-pointer merge scan.
 */
typedef struct {
    vclock_entry_t *entries; /* Pointer to contiguous heap array */
    size_t count;            /* Number of valid entries currently stored */
    size_t capacity;         /* Total allocated capacity */
} vclock_t;

/**
 * Relationship between two vector clocks A and B.
 */
typedef enum {
    VC_EQUAL,       /* A == B: Both clocks have identical nodes and counters */
    VC_ANCESTOR,    /* A < B:  B causally dominates A (B is strictly newer) */
    VC_DESCENDANT,  /* A > B:  A causally dominates B (A is strictly newer) */
    VC_CONCURRENT   /* A || B: Conflict / Siblings (Concurrent divergence) */
} vclock_rel_t;

/* Initial default capacity for a newly allocated vector clock */
#define VCLOCK_INITIAL_CAPACITY 4

/* Maximum recommended entries before Dynamo pruning kicks in */
#define VCLOCK_DEFAULT_MAX_ENTRIES 10

/**
 * Initialize a vector clock to an empty state.
 */
void vclock_init(vclock_t *vc);

/**
 * Free memory held by the vector clock and reset fields to 0.
 */
void vclock_free(vclock_t *vc);

/**
 * Create a deep copy of a vector clock.
 * Returns 0 on success, -1 on allocation failure.
 */
int vclock_copy(vclock_t *dst, const vclock_t *src);

/**
 * Increment the counter for node_id.
 * If node_id is already present:
 *    counter = counter + 1, timestamp updated.
 * If node_id is new:
 *    inserted in sorted order with counter = 1, timestamp recorded.
 * 
 * Returns 0 on success, -1 on allocation failure.
 */
int vclock_increment(vclock_t *vc, uint32_t node_id, uint64_t timestamp);

/**
 * Retrieve the counter for a given node_id.
 * Returns 0 if node_id does not exist in the clock.
 */
uint64_t vclock_get_counter(const vclock_t *vc, uint32_t node_id);

/**
 * Compare two vector clocks A and B.
 * 
 * Returns:
 *   VC_EQUAL      if A == B
 *   VC_ANCESTOR   if A is an ancestor of B (B is strictly newer)
 *   VC_DESCENDANT if A is a descendant of B (A is strictly newer)
 *   VC_CONCURRENT if neither dominates (divergent concurrent writes / siblings)
 */
vclock_rel_t vclock_compare(const vclock_t *a, const vclock_t *b);

/**
 * Prune the oldest entries based on timestamp if count > max_entries.
 * Preserves sorted order of remaining entries.
 * 
 * Returns 0 on success.
 */
int vclock_prune(vclock_t *vc, size_t max_entries);

/**
 * Format the vector clock into a human-readable string:
 * e.g., "[(Node 1: 3), (Node 2: 1)]"
 * 
 * Returns number of characters written, or -1 on buffer overflow.
 */
int vclock_to_string(const vclock_t *vc, char *buf, size_t buflen);

#ifdef __cplusplus
}
#endif

#endif /* DYNAMO_VECTOR_CLOCK_H */
