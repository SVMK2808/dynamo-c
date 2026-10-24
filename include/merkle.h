#ifndef DYNAMO_MERKLE_H
#define DYNAMO_MERKLE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "vector_clock.h"

/**
 * An item input for building a Merkle Tree leaf.
 */

 typedef struct {
    const void *key;
    size_t key_len;
    const void *val;
    size_t val_len;
    const vclock_t *vclock;
 } merkle_item_t;

 /**
  * A node in the Merkle Hash Tree.
  */

  typedef struct merkle_node {
    uint64_t hash;              // Hash of this subtree (or leaf)
    void *key;                  // Non - NULL for leaf nodes; NULL for internal nodes
    size_t key_len;
    struct merkle_node *left;
    struct merkle_node *right; 
  } merkle_node_t;

  /**
   * Merkle Tree container.
   */

   typedef struct {
    merkle_node_t *root;
    size_t leaf_count;
   } merkle_tree_t;

   /**
    * Result of comparing two Merkle trees (out - of - sync keys).
    */

    typedef struct {
        void **keys;
        size_t *key_lens;
        size_t count;
    } merkle_diff_t;

    /**
     * Lifecycle functions.
     */

     void merkle_tree_init(merkle_tree_t *tree);
     void merkle_tree_free(merkle_tree_t *tree);

     /**
      * Builds a deterministic Merkle Tree from an array of items.
      * 1. Sorts items lexicographically by key (so tree topology is deterministic).
      * 2. Computes leaf hashes: HashKey || Val || VClock).
      * 3. Builds a balanced binary tree botton - up with parent hashes: Hash(Left || Right).
      * Returns 0 on success, or -1 on allocation failure.
      */

    int merkle_tree_build(merkle_tree_t *tree, const merkle_item_t *items, size_t count);

    /**
     * Returns the 64 - bit root hash of the tree (or 0 if tree is empty).
     */

    uint64_t merkle_tree_root_hash(const merkle_tree_t *tree);

    /**
     * Traverses two Merkle trees in O(log K) steps and collects all divergent keys.
     * If root hashes match, completes in 1 step with count = 0.
     * Populates out_diff with keys that differ or exist in one tree but not the other.
     * Returns 0 on success, or -1 on allocation failure. 
     */

     int merkle_tree_diff(const merkle_tree_t *tree_a, const merkle_tree_t *tree_b, merkle_diff_t *out_diff);

     /**
      * Frees memeory inside a merkle_diff_t struct.
      */

      void merkle_diff_free(merkle_diff_t *diff);

      #endif //DYNAMO_MERKLE_H