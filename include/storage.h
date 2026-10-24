#ifndef STORAGE_H
#define STORAGE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "vector_clock.h"

#define DEFAULT_STORAGE_BUCKETS 1024

/*
 * A single version of a value, tagged with its vector clock 
 */

 typedef struct {
    void *val;          // Heap - allocated value buffer
    size_t val_len;     // Length of value in bytes
    vclock_t vclock;    // Vector clock capturing this version's causality
 } storage_version_t;


 /*
  * A key entry in the storage engine, holding 1 or more sibling versions.
  */

  typedef struct storage_entry {
    void *key;                      // Heap - allocated key buffer
    size_t key_len;                 // Length of key in bytes

    storage_version_t *versions;    // Dynamic array of active sibling versions
    size_t version_count;           // Number of active sibling versions
    size_t version_capacity;        // Allocated capacity of versions array

    struct storage_entry *next;     // Collision chain in the hash bucket
  } storage_entry_t;

  /*
   * The Local Storage Engine (Hash Table with Separate Chaining).
   */

   typedef struct {
    storage_entry_t **buckets;      // Array of pointers to bucket head entries

    size_t num_buckets;             // Total number of buckets
    size_t total_keys;              // Total distinct keys currently stored
   } storage_engine_t;

   //Lifecycle
   int storage_init(storage_engine_t *engine, size_t num_buckets);
   void storage_free(storage_engine_t *engine);

   /**
    * Write a key-value pair with an associated vector clock.
    *
    * Causality Rules:
    * - If vclock dominates an existing version: replace that obsolete version.
    * - If an existing version dominates vclock: ignore this incoming write
    * - If vclock is concurrent with existing versions: append as a new sibling.
    *
    * Returns 0 on success, -1 on memory allocation error.
    */

   int storage_put(storage_engine_t *engine,
                    const void *key, size_t key_len,
                    const void *val, size_t val_len,
                    const vclock_t *vclock);

   /**
    * Read all active for a key
    * 
    * Returns a pointer to the storage_entry_t for read access, or NULL if key not found.
    */                
   
    const storage_entry_t *storage_get(const storage_engine_t *engine,
                                       const void *key, size_t key_len);

    
    /**
     * Delete a key and all of its versions.
     * Returns true if the key was found and deleted, false otherwise.
     */

     bool storage_delete(storage_engine_t *engine, const void *key, size_t key_len);

#endif
    