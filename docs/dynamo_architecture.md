# Amazon Dynamo: Comprehensive Architectural Specification & Design Document

> **Target Goal:** Reference manual and architectural foundation for implementing a distributed key-value store in C, prepared for Systems / Distributed Systems PhD interviews (IIT Madras RISE Lab, IIT Hyderabad Systems Group).
> **Primary Source:** *Amazon's Dynamo: Highly Available Key-value Store* (DeCandia et al., SOSP 2007).

---

## Table of Contents

1. [Core Design Philosophy &amp; Requirements](#1-core-design-philosophy--requirements)
2. [Architectural Overview &amp; The Six Pillars](#2-architectural-overview--the-six-pillars)
3. [Pillar 1: Consistent Hashing &amp; Virtual Nodes](#3-pillar-1-consistent-hashing--virtual-nodes)
4. [Pillar 2: Replication &amp; Sloppy Quorums (N, R, W)](#4-pillar-2-replication--sloppy-quorums-n-r-w)
5. [Pillar 3: Data Versioning &amp; Vector Clocks](#5-pillar-3-data-versioning--vector-clocks)
6. [Pillar 4: Handling Transient Failures via Hinted Handoff](#6-pillar-4-handling-transient-failures-via-hinted-handoff)
7. [Pillar 5: Anti-Entropy via Merkle Trees](#7-pillar-5-anti-entropy-via-merkle-trees)
8. [Pillar 6: Membership &amp; Decentralized Failure Detection (Gossip)](#8-pillar-6-membership--decentralized-failure-detection-gossip)
9. [End-to-End Request Lifecycles (PUT and GET)](#9-end-to-end-request-lifecycles-put-and-get)
10. [Comparison Matrix: Why Two Recovery Mechanisms?](#10-comparison-matrix-why-two-recovery-mechanisms)
11. [Mapping to C Implementation: Structural Blueprint](#11-mapping-to-c-implementation-structural-blueprint)

---

## 1. Core Design Philosophy & Requirements

### The "Always-Writable" SLA

Traditional enterprise relational and distributed databases prioritize **Consistency** over **Availability** (CP systems under the CAP theorem, e.g., Spanner, Paxos/Raft-based systems). If a network partition occurs, writes to minority partitions are rejected or blocked to prevent dirty reads and split-brain.

Dynamo rejected this assumption based on e-commerce operations:

- A customer must **never** be blocked from adding an item to their shopping cart.
- Rejecting an update or stalling on a disk/network timeout directly translates into lost revenue and poor customer experience.
- Therefore, Dynamo was designed as an **AP system** (Availability and Partition Tolerance): **High Availability for writes at the cost of weak/eventual consistency**.

### Core Guarantees & Assumptions

- **Symmetry:** Every node possesses the exact same responsibilities as its peers. There are no special "coordinator nodes" or single points of failure (unlike Google GFS / Bigtable with master nodes).
- **Decentralization:** Peer-to-peer gossip control plane. No centralized cluster manager (e.g., Apache ZooKeeper, Consul, or etcd).
- **Heterogeneity:** The architecture gracefully handles machines with disparate hardware capabilities via Virtual Nodes.
- **SLA Conformance:** Latency guarantees at the 99.9th percentile (tail latency), typically $\le 300\text{ ms}$ under heavy production load.

---

## 2. Architectural Overview & The Six Pillars

Table 1 from the seminal 2007 paper decomposes Dynamo into six primary building blocks:

```
+-------------------------------------------------------------------------------+
|                                 Client API                                    |
|                       GET(key)  |  PUT(key, val, context)                     |
+-------------------------------------------------------------------------------+
                                       |
+-------------------------------------------------------------------------------+
|                              Coordinator Logic                                |
|        - Consistent Hash Ring Lookup (Top-N Preference List)                 |
|        - Read / Write Dispatcher                                              |
|        - Sloppy Quorum & Read Repair Trigger                                  |
+-------------------------------------------------------------------------------+
        |                                                              |
+-------v-------------------------+          +-------------------------v--------+
|   Data Partitioning & Routing   |          |      Data Versioning             |
|   - Consistent Hashing (Ring)   |          |      - Vector Clocks             |
|   - Virtual Nodes (Tokens)      |          |      - Conflict Detection        |
+---------------------------------+          +----------------------------------+
        |                                                              |
+-------v-------------------------+          +-------------------------v--------+
|   Transient Failure Handling    |          |   Permanent Failure Recovery     |
|   - Sloppy Quorum               |          |   - Merkle Trees                 |
|   - Hinted Handoff              |          |   - Background Anti-Entropy      |
+---------------------------------+          +----------------------------------+
        |                                                              |
+-------v--------------------------------------------------------------v--------+
|                 Membership & Decentralized Failure Detection                  |
|                 - Gossip Protocol (Heartbeats & Node States)                 |
|                 - Seed Nodes for Cluster Bootstrapping                        |
+-------------------------------------------------------------------------------+
                                       |
+-------------------------------------------------------------------------------+
|                            Local Storage Engine                               |
|              Key -> (Value, VectorClock, Timestamp, Metadata)                 |
+-------------------------------------------------------------------------------+
```

### Quick Reference Matrix

| Problem                                | Mechanism                                     | Technical Objective                                                                  |
| :------------------------------------- | :-------------------------------------------- | :----------------------------------------------------------------------------------- |
| **Partitioning**                 | Consistent Hashing with Virtual Nodes         | Dynamic scalability and uniform load distribution across heterogeneous nodes.        |
| **High Availability for Writes** | Vector Clocks with Client/Read Reconciliation | Decouple write throughput from update rates; never reject concurrent writes.         |
| **Replication & Consensus**      | Sloppy Quorum ($N, R, W$)                   | Ensure read/write reachability even when primary replicas are unreachable.           |
| **Transient Node Outages**       | Hinted Handoff                                | Replicas outside the top-$N$ temporarily accept writes and route back on recovery. |
| **Permanent Divergence**         | Anti-Entropy via Merkle Trees                 | Detect replica divergence using minimal cross-network payload exchanges.             |
| **Membership & Node Liveness**   | Gossip-based Protocol                         | Eliminate centralized coordinators; ensure eventual cluster view convergence.        |

---

## 3. Pillar 1: Consistent Hashing & Virtual Nodes

### The Consistent Hash Ring

The output range of a cryptographic hash function (e.g., MD5, SHA-1, or MurmurHash3) is treated as a circular loop or "ring" from $0$ to $2^b - 1$ (where $b = 32$, $64$, or $128$).

```
                         Token 0
                     /              \
             Node C                     Node A
         [Token: 2^30]              [Token: 2^31]
                    \                /
                         Node B
                     [Token: 2^31 + 2^30]
```

1. **Routing Rule:** To locate which node is responsible for key $K$:

   $$
   \text{Token}(K) = \text{Hash}(K)
   $$

   Traverse the ring clockwise until encountering the first node whose assigned token $\ge \text{Token}(K)$.
2. **Virtual Nodes (vnodes):**
   Assigning each physical server a single point on the ring causes two critical problems:

   - Non-uniform distribution of keys (some nodes receive massive ranges).
   - Node failure dumps 100% of the failed node's traffic directly onto its immediate clockwise neighbor.

   **Solution:** A single physical server is assigned $T$ distinct tokens (typically $T \in [32, 256]$).

   - If physical machine $M_1$ has $32$ vnodes, it occupies $32$ discrete points across the circumference.
   - When $M_1$ crashes, its $32$ ranges are handed off to multiple distinct physical machines across the ring, distributing the failover load evenly.
   - Nodes with larger hardware capacity can be assigned proportionally more virtual nodes.

### The Preference List

For any key $K$:

- We identify the primary coordinate on the ring via $\text{Hash}(K)$.
- We scan clockwise to collect the first $N$ **distinct physical nodes** (skipping tokens belonging to physical servers already included).
- This ordered list of $N$ physical machines is the **Preference List** for key $K$.

---

## 4. Pillar 2: Replication & Sloppy Quorums (N, R, W)

### Quorum Parameters

Replication is governed by three configurable parameters:

- **$N$ (Replica Count):** The number of physical nodes that must store a copy of the key (typically $N=3$).
- **$W$ (Write Quorum):** The number of replica acknowledgments required before a write is confirmed to the client (typically $W=2$).
- **$R$ (Read Quorum):** The number of replica responses required before returning data to the client (typically $R=2$).

### Strict Quorum vs. Sloppy Quorum

- **Strict Quorum ($R + W > N$):** In traditional quorum systems (e.g., Gifford 1979), reads and writes must overlap on at least one replica:
  $$
  (R + W) > N \implies (\text{Read Set} \cap \text{Write Set}) \neq \emptyset
  $$

  However, in strict quorum, if $N=3$ and $2$ nodes in the preference list are partitioned/dead, writes with $W=2$ **fail**.
- **Sloppy Quorum:** Dynamo relaxes strict adherence to the preference list:
  - If the top $N$ nodes in the preference list are unreachable, the coordinator **continues walking clockwise along the ring** beyond the top $N$, finding the first $W$ healthy nodes (nodes $N+1$, $N+2$, etc.).
  - This guarantees write availability during extensive network splits.

---

## 5. Pillar 3: Data Versioning & Vector Clocks

Because Dynamo accepts writes to alternate nodes during network splits, concurrent writes to the same key can occur independently on different partitions. Wall-clock timestamps cannot resolve these writes due to clock skew and NTP drift.

### Vector Clock Definition

A Vector Clock is an array of pairs:

$$
VC = \{ (S_1, c_1), (S_2, c_2), \dots, (S_k, c_k) \}
$$

where $S_i$ is a node identifier and $c_i$ is a monotonically increasing local counter.

### Causality & Comparison Rules

Given two versions of an object with vector clocks $V_1$ and $V_2$:

1. **$V_1$ Dominates $V_2$ ($V_1$ is an ancestor of $V_2$, $V_1 \le V_2$):**
   $$
   \forall S \in V_1: V_1[S] \le V_2[S] \quad \text{and} \quad \exists S : V_1[S] < V_2[S]
   $$

   *(The system can safely overwrite $V_1$ with $V_2$; no conflict exists).*
2. **$V_1$ and $V_2$ are Concurrent (Conflict / Siblings, $V_1 \parallel V_2$):**
   $$
   \exists S_a : V_1[S_a] > V_2[S_a] \quad \text{and} \quad \exists S_b : V_2[S_b] > V_1[S_b]
   $$

   *(Neither clock causally precedes the other. Both versions must be retained as **siblings**).*

```
                     Initial Write: V1
                      VC: [(NodeA, 1)]
                             |
             +---------------+---------------+
             | (Network Partition Occurs)    |
             v                               v
     Client 1 updates via A          Client 2 updates via B
     Val: "Apples"                   Val: "Oranges"
     VC: [(A, 2)]                    VC: [(A, 1), (B, 1)]
             \                               /
              +--------------+--------------+
                             | (Partition Heals)
                             v
                     Client executes GET()
      Dynamo detects conflict: [(A,2)] || [(A,1), (B,1)]
      Coordinator returns BOTH siblings to Client.
                             |
                     Client reconciles
              Val: "Apples, Oranges" (merged)
             VC: [(A, 3), (B, 1)] written back
```

### Vector Clock Truncation (Pruning)

As multiple nodes coordinate updates over months, vector clock sizes can grow unbounded. Dynamo maintains a timestamp with each `(Node, Counter)` pair. If the length of the vector clock exceeds a threshold (e.g., 10 entries), the oldest entry is pruned.

---

## 6. Pillar 4: Handling Transient Failures via Hinted Handoff

When Sloppy Quorum routes a write to a node outside the primary preference list, **Hinted Handoff** ensures the data eventually reaches its rightful owner.

```
+---------------+           +---------------+           +---------------+
|  Coordinator  |           |    Node B     |           |    Node D     |
|    Node A     |           | (Primary N=2) |           |   (Fallback)  |
+---------------+           +---------------+           +---------------+
        |                           |                           |
        |--- PUT(K, V) ------------>| (Unreachable / Timeout)   |
        |                           x                           |
        |--- PUT(K, V, Hint="NodeB") -------------------------->|
        |                                                       | Stores locally in
        |                                                       | "Hint Store"
        |                                                       |
        |                  [Node B recovers]                    |
        |                           |                           |
        |                           |<--- Periodic Heartbeat ---|
        |                           |     (B is healthy!)       |
        |                           |<--- Replay Hint (K, V) ---|
        |                           |--- ACK ------------------>|
        |                           |                           | Deletes local hint
```

1. **Tagging:** Node $D$ receives the write marked with metadata: `TargetOwner = NodeB`.
2. **Isolation:** Node $D$ saves this record in a dedicated **Hint Store** (separate from its own regular key-value data).
3. **Recovery Polling:** A background daemon on Node $D$ periodically verifies the reachability of Node $B$ (via gossip or direct ping).
4. **Handoff:** Once Node $B$ is confirmed reachable, Node $D$ transmits the stored object to Node $B$. Upon receiving confirmation, Node $D$ purges the hint from its local storage.

---

## 7. Pillar 5: Anti-Entropy via Merkle Trees

### Why Hinted Handoff is Not Enough

Hinted handoff fails if:

- The fallback node crashes before delivering its hints.
- A primary node was offline for an extended duration, causing hints to expire or fill up disk limits.
- A node joins the cluster and needs to synchronize ranges it has inherited.

To guarantee eventual consistency after permanent failures or lost hints, Dynamo utilizes **Anti-Entropy** with **Merkle Trees**.

### Merkle Tree Construction

A Merkle Tree is a cryptographic binary tree where:

- Every leaf node represents a hash of a key-value pair: $\text{Hash}(K || V || VC)$.
- Every internal node is the hash of its concatenated children: $\text{Hash}(\text{LeftChild} || \text{RightChild})$.

```
                            Root Hash: H_0
                           /              \
                   Node H_1                Node H_2
                  /        \              /        \
              Leaf A      Leaf B      Leaf C      Leaf D
              Hash(K1)    Hash(K2)    Hash(K3)    Hash(K4)
```

### The Synchronization Protocol

Each node maintains a separate Merkle tree for each virtual node token range it replicates.

1. **Step 1 (Root Comparison):** Node 1 and Node 2 exchange the root hashes of their trees for a common key range.
   - If $\text{Root}_1 == \text{Root}_2$: The two replicas are identical. Sync completes immediately in **$1$ network roundtrip**.
2. **Step 2 (Branch Traversal):** If $\text{Root}_1 \neq \text{Root}_2$:
   - The nodes exchange child hashes for the next level down.
   - Branches that match are discarded immediately.
   - The nodes recurse down the tree **only along branches with mismatched hashes**.
3. **Step 3 (Targeted Key Sync):** Once leaf nodes are reached, only the specific out-of-sync keys are transferred across the network.

---

## 8. Pillar 6: Membership & Decentralized Failure Detection (Gossip)

### Decentralized Membership

Dynamo avoids centralized registries (e.g., ZooKeeper). Membership is managed via a decentralized **Gossip Protocol**:

- **Gossip Rounds:** Every $T$ seconds (e.g., 1s), each node selects a random peer from its known node list.
- **State Exchange:** The node transmits its internal membership table, which contains:
  - Node IDs and IP/Port endpoints.
  - Assigned virtual node tokens.
  - Generation timestamps / incarnation counters.
- **Convergence:** Both nodes merge their membership tables. In an $M$-node cluster, new membership information propagates across the entire network in $\mathcal{O}(\log M)$ gossip rounds.
- **Seed Nodes:** To prevent cluster partitioning during bootstrap (where two sets of nodes boot up unaware of each other), a small set of well-known "Seed Nodes" is defined in a static configuration file. All nodes gossip with seed nodes initially.

### Failure Detection

Dynamo combines two levels of failure detection:

1. **Local Dynamic Detection:** If Node $A$ sends a client read/write to Node $B$ and gets a socket timeout, Node $A$ locally marks Node $B$ as unreachable and immediately redirects requests to alternate nodes (Sloppy Quorum).
2. **Global Gossip Detection:** If a node fails to respond to multiple gossip rounds from multiple peers, the cluster gradually updates the node's state to `DEAD`.

---

## 9. End-to-End Request Lifecycles (PUT and GET)

### The `PUT(key, value, context)` Protocol

```
Client             Coordinator (Node A)          Node B (Replica)       Node C (Replica)
  |                         |                           |                      |
  |-- PUT(K, V, ctx) ------>|                           |                      |
  |                         |-- Increment VC[A]         |                      |
  |                         |-- Write Local Storage     |                      |
  |                         |                           |                      |
  |                         |-- PUT(K, V, VC) --------->|                      |
  |                         |-- PUT(K, V, VC) -------------------------------->|
  |                         |                           |                      |
  |                         |<-- ACK -------------------|                      |
  |                         |   [W=2 Quorum Satisfied]  |                      |
  |<-- 200 OK (New VC) -----|                           |                      |
  |                         |                                                  |
  |                         |<-- ACK (Async/Background) -----------------------|
```

1. **Entry:** Client sends `PUT(key, value, context)` to any node.
2. **Routing:** If the receiving node is not in the key's preference list, it forwards the request to the first node in the preference list (the Coordinator).
3. **Clock Advancement:** The coordinator reads the incoming vector clock from `context`, increments its own entry in the vector clock:
   $$
   VC_{\text{new}}[A] = VC_{\text{ctx}}[A] + 1
   $$
4. **Local Write:** Coordinator writes the new tuple $(K, V, VC_{\text{new}})$ to its local storage.
5. **Parallel Replication:** Coordinator sends the write payload concurrently to the top $N-1$ reachable preference list nodes.
6. **Quorum Acknowledgment:** As soon as $W - 1$ remote replicas acknowledge the write (giving $W$ total including the local write), the coordinator returns a success status to the client.

---

### The `GET(key)` Protocol (with Read Repair)

```
Client             Coordinator (Node A)          Node B (Replica)       Node C (Replica)
  |                         |                           |                      |
  |-- GET(K) -------------->|                           |                      |
  |                         |-- Read Local Storage      |                      |
  |                         |-- GET(K) ---------------->|                      |
  |                         |-- GET(K) --------------------------------------->|
  |                         |                           |                      |
  |                         |<-- (V2, VC:[(A,2)]) ------|                      |
  |                         |   [R=2 Quorum Satisfied]  |                      |
  |                         |                           |                      |
  |                         |-- Reconcile Versions:     |                      |
  |                         |   Local was [(A,1)],      |                      |
  |                         |   Node B has [(A,2)].     |                      |
  |                         |   Node B dominates!       |                      |
  |<-- 200 OK (V2, VC) -----|                           |                      |
  |                         |                                                  |
  |                         |-- Async Read Repair:      |                      |
  |                         |   Write V2 locally        |                      |
  |                         |   Update Node C if stale  |                      |
```

1. **Entry:** Client issues `GET(key)` to the coordinator.
2. **Parallel Dispatch:** Coordinator queries its local storage and sends asynchronous read requests to the preference list nodes.
3. **Quorum Wait:** Coordinator waits until $R$ responses arrive.
4. **Version Reconciliation:**
   - **Case A (Dominance):** One version's vector clock causally dominates all other returned versions. The coordinator selects this version.
   - **Case B (Conflict):** Multiple versions have concurrent, non-dominating vector clocks. The coordinator gathers **all siblings** and returns them to the client. The client application performs domain-level merge (e.g., union of items) and writes back the resolved value.
5. **Asynchronous Read Repair:** If any replica returned an obsolete, causally dominated version, the coordinator asynchronously dispatches the latest version to that stale node in the background.

---

## 10. Comparison Matrix: Why Two Recovery Mechanisms?

Students and interview candidates frequently ask:

> *"Why does Dynamo need both Hinted Handoff AND Merkle Tree Anti-Entropy?"*

| Dimension                   | **Hinted Handoff**                                                     | **Merkle Tree Anti-Entropy**                                     |
| :-------------------------- | :--------------------------------------------------------------------------- | :--------------------------------------------------------------------- |
| **Failure Target**    | **Transient / Short-lived** outages (milliseconds to minutes).         | **Permanent / Long-term** divergence (hours, days, disk loss).   |
| **Trigger**           | Reactive (invoked during failed write dispatch in Sloppy Quorum).            | Proactive (background daemon running periodically).                    |
| **Storage Location**  | Stored on the fallback node's temporary hint queue.                          | Computed directly over the node's permanent storage partitions.        |
| **Network Cost**      | Tiny: One-way direct push of individual updates.                             | Sub-linear: Exchange hashes of ranges; only payload diffs transmitted. |
| **Failure Edge Case** | If the fallback node dies before handoff, data is lost without Anti-Entropy. | Catches all dropped hints, cold node replacements, and silent bit-rot. |

---

## 11. Mapping to C Implementation: Structural Blueprint

When implementing this architecture in C, modular separation is essential to prevent spaghetti concurrency and memory leaks:

```
dynamo_c/
├── include/
│   ├── ring.h          # Hash ring, tokens, preference list binary search
│   ├── vector_clock.h  # VC struct, comparison (DOMINATES, SIBLINGS), serialize
│   ├── storage.h       # Local key-value engine (WAL + Hash Index / SkipList)
│   ├── quorum.h        # Coordinator state machine, N/R/W quorum collector
│   ├── hinted.h        # Hint store and background delivery thread
│   ├── merkle.h        # Merkle tree building, range hashing, tree comparison
│   ├── gossip.h        # Membership tables, heartbeat protocol, failure detector
│   └── network.h       # Binary TLV framing, socket non-blocking event loops
├── src/
│   ├── ring.c
│   ├── vector_clock.c
│   ├── storage.c
│   ├── quorum.c
│   ├── hinted.c
│   ├── merkle.c
│   ├── gossip.c
│   ├── network.c
│   └── main.c
├── tests/              # Unit tests for Vector Clocks, Ring, Merkle Trees
└── Makefile
```

### Core C Data Structures Preview

#### 1. Vector Clock

```c
typedef struct {
    uint32_t node_id;
    uint64_t counter;
    uint64_t timestamp;
} vclock_entry_t;

typedef struct {
    uint16_t count;
    vclock_entry_t entries[MAX_VCLOCK_ENTRIES];
} vector_clock_t;

typedef enum {
    VC_EQUAL,
    VC_BEFORE,      // Clock A is ancestor of Clock B
    VC_AFTER,       // Clock A dominates Clock B
    VC_CONCURRENT   // Conflict / Siblings
} vc_relation_t;
```

#### 2. Virtual Node Ring

```c
typedef struct {
    uint32_t token;         // Position on hash ring [0, 2^32 - 1]
    uint32_t physical_id;   // Physical server identifier
    char ip[16];
    uint16_t port;
} vnode_t;

typedef struct {
    uint32_t total_vnodes;
    vnode_t vnodes[MAX_TOTAL_VNODES]; // Kept sorted by token for O(log V) binary search
} hash_ring_t;
```

#### 3. Wire Message Header

```c
#pragma pack(push, 1)
typedef struct {
    uint32_t magic;         // Protocol magic (e.g., 0x44594E41 = 'DYNA')
    uint16_t msg_type;      // GET_REQ, GET_RESP, PUT_REQ, PUT_RESP, GOSSIP, MERKLE_SYNC
    uint32_t payload_len;   // Byte length of payload following header
    uint32_t correlation_id;// Matches asynchronous responses to requests
} msg_header_t;
#pragma pack(pop)
```

---

## 12. Interview Defense: What Faculty Will Ask

1. **"Why not Raft or Paxos instead of Vector Clocks and Quorums?"**
   - *Answer:* Raft and Paxos require a stable leader and a strict majority ($> N/2$) to make progress. If a network partition isolates a minority group, writes in that group are completely blocked. Dynamo prioritizes customer write availability above all else (AP), opting to accept writes on partitions and resolve conflicts later.
2. **"What happens if vector clocks grow indefinitely?"**
   - *Answer:* Dynamo implements vector clock truncation based on timestamped entries. When the clock size exceeds a threshold, the oldest entry is pruned. While rare, this can lead to false concurrency detections, which Dynamo's sibling reconciliation safely handles.
3. **"How do you prevent two partitions from joining with different ring tokens?"**
   - *Answer:* Dynamo relies on explicit administrator actions and Seed Nodes for ring changes, gossip for membership propagation, and logical generation numbers to detect and order configuration updates.
