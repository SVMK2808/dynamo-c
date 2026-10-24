#include "../include/merkle.h"
#include <stdlib.h>
#include <string.h>

#define FNV_OFFSET_BASIS 14695981039346656037ULL
#define FNV_PRIME        1099511628211ULL

static uint64_t fnv1a_update(uint64_t hash, const void *data, size_t len){
    const uint8_t *p = (const uint8_t *)data;
    for(size_t i = 0; i < len; i++){
        hash ^= (uint64_t)p[i];
        hash *= FNV_PRIME;
    }

    return hash;
}

static inline uint64_t hash_combine(uint64_t left, uint64_t right){
    uint64_t h = FNV_OFFSET_BASIS;
    h = fnv1a_update(h, &left, sizeof(left));
    h = fnv1a_update(h, &right, sizeof(right));
    return h;
}

static uint64_t compute_leaf_hash(const merkle_item_t *item){
    uint64_t h = FNV_OFFSET_BASIS;
    if(item -> key && item -> key_len > 0){
        h = fnv1a_update(h, item -> key, item -> key_len);
    }

    if(item -> val && item -> val_len > 0){
        h = fnv1a_update(h, item -> val, item -> val_len);
    }

    if(item -> vclock){
        for(size_t i = 0; i < item -> vclock -> count; i++){
            uint32_t nid = item -> vclock -> entries[i].node_id;
            uint64_t cnt = item -> vclock -> entries[i].counter;
            h = fnv1a_update(h, &nid, sizeof(nid));
            h = fnv1a_update(h, &cnt, sizeof(cnt));
        }
    }

    return h;
}

static int item_cmp(const void *a, const void *b){
    const merkle_item_t *ia = (const merkle_item_t *)a;
    const merkle_item_t *ib = (const merkle_item_t *)b;
    size_t min_len = (ia -> key_len < ib -> key_len) ? ia -> key_len : ib -> key_len;
    int cmp = memcmp(ia -> key, ib -> key, min_len);
    if(cmp != 0) return cmp;
    if(ia -> key_len < ib -> key_len) return -1;
    if(ia -> key_len > ib -> key_len) return 1;
    return 0;
}

void merkle_tree_init(merkle_tree_t *tree){
    if(!tree) return;
    tree -> root = NULL;
    tree -> leaf_count = 0;
}

static void free_node(merkle_node_t *node){
    if(!node) return;
    if(node -> left) free_node(node -> left);
    if(node -> right) free_node(node -> right);
    if(node -> key) free(node -> key);
    free(node);
}

void merkle_tree_free(merkle_tree_t *tree){
    if(!tree) return;
    if(tree -> root){
        free_node(tree -> root);
        tree -> root = NULL;
    }

    tree -> leaf_count = 0;
}

uint64_t merkle_tree_root_hash(const merkle_tree_t *tree){
    if(!tree || !tree -> root) return 0;
    return tree -> root -> hash;
}

static merkle_node_t *build_recursive(merkle_node_t **leaves, size_t start, size_t end){
    if(start == end){
        return leaves[start];
    }

    size_t mid = start + (end - start) / 2;
    merkle_node_t *left = build_recursive(leaves, start, mid);
    merkle_node_t *right = build_recursive(leaves, mid + 1, end);

    merkle_node_t *parent = calloc(1, sizeof(merkle_node_t));
    if(!parent) return NULL;

    parent -> left = left;
    parent -> right = right;
    parent -> hash = hash_combine(left -> hash, right -> hash);
    return parent;
}

int merkle_tree_build(merkle_tree_t *tree, const merkle_item_t *items, size_t count){
    if(!tree) return -1;

    merkle_tree_free(tree);

    if(!items || count == 0){
        return 0; // Empty tree is valid
    }

    //1. Sort items lexicographically by key to ensure deterministic tree layout.
    merkle_item_t *sorted_items = malloc(sizeof(merkle_item_t) * count);
    if(!sorted_items) return -1;
    memcpy(sorted_items, items, sizeof(merkle_item_t) * count);
    qsort(sorted_items, count, sizeof(merkle_item_t), item_cmp);

    //2. Allocate and compute leaf nodes.
    merkle_node_t **leaves = malloc(sizeof(merkle_node_t *) * count);
    if(!leaves){
        free(sorted_items);
        return -1;
    }

    for(size_t i = 0; i < count; i++){
        merkle_node_t *leaf = calloc(1, sizeof(merkle_node_t));
        if(!leaf){
            for(size_t j = 0; j < i; j++) free_node(leaves[j]);
            free(leaves);
            free(sorted_items);
            return -1;
        }

        leaf -> hash = compute_leaf_hash(&sorted_items[i]);
        leaf -> key = malloc(sorted_items[i].key_len);
        if(!leaf -> key){
            free(leaf);
            for(size_t j = 0; j < i; j++) free_node(leaves[j]);
            free(leaves);
            free(sorted_items);
            return -1;
        }

        memcpy(leaf -> key, sorted_items[i].key, sorted_items[i].key_len);
        leaf -> key_len = sorted_items[i].key_len;

        leaves[i] = leaf;
    }

    free(sorted_items);

    //3. Build tree recursively.
    tree -> root = build_recursive(leaves, 0, count - 1);
    tree -> leaf_count = count;

    free(leaves);
    return 0;
}

static int diff_add_key(merkle_diff_t *diff, const void *key, size_t key_len){
    if(!diff || !key || key_len == 0) return 0;

    for(size_t i = 0; i < diff -> count; i++){
        if(diff -> key_lens[i] == key_len && memcmp(diff -> keys[i], key, key_len) == 0){
            return 0; //already in diff list
        }
    }

    void **new_keys = realloc(diff -> keys, sizeof(void *) * (diff -> count + 1));
    size_t *new_lens = realloc(diff -> key_lens, sizeof(size_t) * (diff -> count + 1));
    if(!new_keys || !new_lens) return -1;

    diff -> keys = new_keys;
    diff -> key_lens = new_lens;

    diff -> keys[diff -> count] = malloc(key_len);
    if(!diff -> keys[diff -> count]) return -1;
    memcpy(diff -> keys[diff -> count], key, key_len);
    diff -> key_lens[diff -> count] = key_len;
    diff -> count++;
    return 0;
}

static void collect_all_leaves(const merkle_node_t *node, merkle_diff_t *diff){
    if(!node) return;
    if(node -> key){
        diff_add_key(diff, node -> key, node -> key_len);
        return;
    }

    collect_all_leaves(node -> left, diff);
    collect_all_leaves(node -> right, diff);
}

static void diff_recursive(const merkle_node_t *a, const merkle_node_t *b, merkle_diff_t *diff){
    if(!a && !b) return;

    if(!a && b){
        collect_all_leaves(b, diff);
        return;
    }

    if(a && !b){
        collect_all_leaves(a, diff);
        return;
    }

    //Merkle tree optimization: if subtree hashes match, they are 100% identical!
    if(a -> hash == b -> hash){
        return;
    }

    // Both are leaves with different hashes
    if(a -> key && b -> key){
        if(a -> key_len == b -> key_len && memcmp(a -> key, b -> key, a -> key_len) == 0){
            // Same key, but value or vector clock differed
            diff_add_key(diff, a -> key, a -> key_len);
        }else{
            diff_add_key(diff, a -> key, a -> key_len);
            diff_add_key(diff, b -> key, b -> key_len);
        }

        return;
    }

    // Both are internal nodes
    if(!a -> key && !b -> key){
        diff_recursive(a -> left, b -> left, diff);
        diff_recursive(a -> right, b -> right, diff);
        return;
    }

    // One is a leaf and one is an internal node.
    collect_all_leaves(a, diff);
    collect_all_leaves(b, diff);
}

int merkle_tree_diff(const merkle_tree_t *tree_a, const merkle_tree_t *tree_b, merkle_diff_t *out_diff){
    if(!out_diff) return -1;

    memset(out_diff, 0, sizeof(merkle_diff_t));

    const merkle_node_t *root_a = tree_a ? tree_a -> root : NULL;
    const merkle_node_t *root_b = tree_b ? tree_b -> root : NULL;

    diff_recursive(root_a, root_b, out_diff);
    return 0;
}

void merkle_diff_free(merkle_diff_t *diff){
    if(!diff) return;

    for(size_t i = 0; i < diff -> count; i++){
        if(diff -> keys[i]){
            free(diff -> keys[i]);
        }
    }

    if(diff -> keys) free(diff -> keys);
    if(diff -> key_lens) free(diff -> key_lens);

    diff -> keys = NULL;
    diff -> key_lens = NULL;
    diff -> count = 0;
}