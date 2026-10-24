# Dynamo: Amazon's Distributed Key-Value Store in Pure C

[![Language](https://img.shields.io/badge/Language-C11-00599C.svg?style=flat-square&logo=c)](https://en.wikipedia.org/wiki/C11_(C_standard_revision))
[![Standard](https://img.shields.io/badge/Standard-POSIX.1--2008-red.svg?style=flat-square)](https://pubs.opengroup.org/onlinepubs/9699919799/)
[![Sanitizers](https://img.shields.io/badge/Sanitizers-ASan%20%7C%20UBSan%20Clean-brightgreen.svg?style=flat-square)](https://clang.llvm.org/docs/AddressSanitizer.html)
[![Architecture](https://img.shields.io/badge/Architecture-SOSP%202007%20Dynamo-orange.svg?style=flat-square)](amazon-dynamo-sosp2007.pdf)
[![Integration](https://img.shields.io/badge/Interface-Native%20POSIX%20Shell-purple.svg?style=flat-square)](POSIX%20Shell/)

A ground-up implementation of **Amazon's Dynamo distributed storage architecture** as detailed in the landmark SOSP 2007 paper:
> **"Dynamo: Amazon’s Highly Available Key-Value Store"**  
> *Giuseppe DeCandia, Deniz Hastorun, Madan Jampani, Gaurav Kakulapati, Avinash Lakshman, Alex Pilchin, Swaminathan Sivasubramanian, Peter Vosshall and Werner Vogels (Amazon.com)*  
> *ACM Symposium on Operating Systems Principles (SOSP '07)*

Engineered in pure C99/C11 using only POSIX sockets and POSIX threads (`pthreads`) with **zero external dependencies**. Includes an integrated, native **POSIX Shell** access layer that treats distributed cluster operations (`connect`, `ping`, `put`, `get`) as first-class builtins with full pipeline (`|`) and redirection support.

---

## Table of Contents
1. [Theoretical Foundations & Distributed Guarantees](#theoretical-foundations--distributed-guarantees)
2. [End-to-End System Architecture](#end-to-end-system-architecture)
3. [Core Module Breakdown](#core-module-breakdown)
4. [Sloppy Quorums & Hinted Handoff Lifecycle](#sloppy-quorums--hinted-handoff-lifecycle)
5. [Anti-Entropy via Merkle Trees](#anti-entropy-via-merkle-trees)
6. [POSIX Shell Integration](#posix-shell-integration)
7. [Building and Testing](#building-and-testing)
8. [Multi-Node Cluster Walkthrough](#multi-node-cluster-walkthrough)
9. [Systems Defense: Key Invariants & Trade-Offs](#systems-defense-key-invariants--trade-offs)

---

## Theoretical Foundations & Distributed Guarantees

Dynamo was conceived around Amazon's core retail requirement: **an always-writable shopping cart**. A customer must never be denied an "Add to Cart" action due to network partitions or server crashes.

| Guarantee | Dynamo Classification | Design Rationale |
|---|---|---|
| **CAP Theorem** | **AP** (Available / Partition Tolerant) | Sacrifices strict consistency ($C$) to guarantee $99.999\%$ write availability ($A$) during network partitions ($P$). |
| **PACELC Theorem** | **PA / EL** | If **P**artition: **A**vailability over **C**onsistency; **E**lse: **L**atency over **C**onsistency. Writes do not wait for all replicas to commit. |
| **Consistency Model** | **Eventual Consistency** with Monotonic Read-Your-Writes | Concurrent conflicting writes branch into **siblings** tracked via Vector Clocks and reconciled asynchronously on read. |
| **Membership & Failure** | **Decentralized Gossip Protocol** | Zero single point of failure (SPOF); no master, ZooKeeper, or Paxos consensus lease required. |

---

## End-to-End System Architecture

```
                                  ┌────────────────────────┐
                                  │      Client Layer      │
                                  │  (POSIX Dynamo Shell)  │
                                  └───────────┬────────────┘
                                              │ TCP Wire Protocol
                                              ▼
                                 ┌───────────────────────────┐
                                 │   Coordinator (Node 1)    │
                                 │  - Computes Preference    │
                                 │    List for Key (N=3)     │
                                 │  - Increments VClock      │
                                 └─────┬───────────────┬─────┘
                     Write Local (1)   │               │ TCP Async Replica Write
                                       ▼               ▼
                           ┌──────────────┐         ┌───────────────────────────┐
                           │ Node 1 Store │         │    Node 2 (Port 9002)     │
                           └──────────────┘         │   - Down / Unreachable    │
                                                    └─────────────┬─────────────┘
                                                                  │ Socket Fail
                                                                  ▼
                                                    ┌───────────────────────────┐
                                                    │ Buffer Hinted Handoff on  │
                                                    │ Node 1 for target Node 2  │
                                                    └─────────────┬─────────────┘
                                                                  │ Gossip detects
                                                                  │ recovery
                                                                  ▼
                                                    ┌───────────────────────────┐
                                                    │ Flush Hint directly into  │
                                                    │ Node 2 Storage Engine     │
                                                    └───────────────────────────┘
```

---

## Core Module Breakdown

The codebase is organized into modular subsystems under [`src/`](src/) and [`include/`](include/):

```
Dynamo/
├── include/
│   ├── coordinator.h     # Quorum coordinator (N, R, W) & sloppy quorum logic
│   ├── gossip.h          # Decentralized heartbeat gossip & failure detection
│   ├── hint.h            # Hinted handoff storage & TCP flush engine
│   ├── merkle.h          # Hierarchical Merkle trees for anti-entropy sync
│   ├── network.h         # POSIX socket transport & non-blocking framing
│   ├── protocol.h        # Binary wire protocol codec & message types
│   ├── ring.h            # 2^32 consistent hash ring with virtual nodes
│   ├── storage.h         # In-memory engine with vector-clock sibling lists
│   └── vector_clock.h    # Vector clock causality engine (Dominates, Equal, Conflict)
├── src/
│   ├── coordinator.c
│   ├── gossip.c
│   ├── hint.c
│   ├── merkle.c
│   ├── network.c
│   ├── protocol.c
│   ├── ring.c
│   ├── storage.c
│   ├── vector_clock.c
│   ├── server.c          # Multi-node daemon binary (dynamo_server)
│   └── cli.c             # Standalone CLI client (dynamo_cli)
├── POSIX Shell/          # Custom POSIX Shell with native Dynamo builtins
└── tests/                # Automated verification suites with ASan / UBSan
```

### 1. Vector Clocks (`vector_clock.c`)
- **Structure:** Array of `(node_id, counter, timestamp)` tuples, sorted by `node_id`.
- **Causality States:** Evaluates strict causal dominance ($A$ descends from $B$, $B$ descends from $A$), exact equality ($A = B$), or concurrent branching ($A \parallel B$ producing siblings).
- **Pruning Heuristic:** Caps vector clock size to avoid metadata bloat using timestamp-based truncation.

### 2. Consistent Hash Ring (`ring.c`)
- **Key Ring:** Uniform distribution across a $2^{32}-1$ integer ring using MD5 hashing.
- **Virtual Nodes (Vnodes):** Configured with 32 vnodes per physical node (`node_id#vnode_idx`) to prevent hash skew and hot-spotting.
- **Preference List:** Ring-walking algorithm that skips co-located virtual nodes to return the top-$N$ distinct physical nodes responsible for a key.

### 3. Versioned Storage Engine (`storage.c`)
- **Hash Table:** Fixed-bucket table with separate chaining for collision resolution.
- **Sibling Preservation:** Each storage bucket entry contains a dynamic array of concurrent versions `versions[]`.
- **Dominance Overwrites:** When a write arrives whose vector clock causally dominates existing versions, old versions are freed and replaced. Concurrent writes are preserved as siblings for client-side resolution.

### 4. Binary Wire Protocol (`protocol.c`)
- **Framing:** 16-byte fixed binary header containing:
  - `magic` (`0xDYNA`)
  - `version` (`1`)
  - `msg_type` (`PING`, `PONG`, `PUT_REQ`, `PUT_RESP`, `GET_REQ`, `GET_RESP`, `GOSSIP`, `MERKLE_SYNC`)
  - `flags` (e.g., `FLAG_HINTED_HANDOFF`)
  - `correlation_id` (matches asynchronous requests and responses)
  - `payload_len` (variable payload size)
- **Serialization:** Zero-copy binary packing for strings, vector clock arrays, and sibling version arrays.

### 5. Network Transport (`network.c`)
- **POSIX Sockets:** Robust TCP socket wrappers with `SO_REUSEADDR`, proper buffer framing, and loopback support.
- **Header Synchronization:** Handles partial TCP reads and writes using strict byte-count loops.

### 6. Quorum Coordinator (`coordinator.c`)
- **Tunable Parameters:** Configurable $N$ (replication factor), $W$ (write quorum size), and $R$ (read quorum size). Typically $N=3, W=2, R=2$, satisfying $R + W > N$.
- **Read Repair:** Reads fetch versions from $R$ replicas, reconcile siblings locally, and update stale replicas.

### 7. Sloppy Quorums & Hinted Handoff (`hint.c`)
- **High Write Availability:** If primary nodes in the preference list are down, the coordinator accepts writes, buffers them in a thread-safe `hint_store_t`, and fulfills $W$.
- **Target Routing:** Hints retain the original intended recipient node ID and vector clock.

### 8. Anti-Entropy with Merkle Trees (`merkle.c`)
- **Hierarchical Hashing:** Fixed-depth binary Merkle tree over the key hash space.
- **Difference Isolation:** Synchronizes replicas by comparing root hashes. If roots differ, traversal identifies the exact discordant key range in $O(\log K)$ network exchanges without transferring key-value payloads.

### 9. Gossip Protocol & Failure Detection (`gossip.c`)
- **Decentralized Membership:** Periodic peer-to-peer heartbeat gossip across seed nodes.
- **Accrual Failure Detection:** Tracks heartbeat ticks and transitions peers between `ALIVE`, `SUSPECT`, and `DEAD` states.
- **Automatic Drainage:** Gossip worker threads actively monitor offline peers with pending hints and trigger asynchronous batch delivery upon node reboot.

---

## Sloppy Quorums & Hinted Handoff Lifecycle

Below is the exact execution trace verified across independent OS processes in our test cluster:

```
[NODE 1 (Port 9001)]                     [NODE 2 (Port 9002)]                     [NODE 3 (Port 9003)]
         │                                        │                                        │
         │  ◄── Node 2 crashes (SIGINT) ──────────X                                        │
         │                                                                                 │
Client PUT "cart_304"                                                                      │
         ├─────────────────────────────── Socket Fail ────────────────────────────────────►│
         │                                                                                 │
         ├─── Writes locally (Ack 1)                                                       │
         ├─── Buffers Hint for Node 2                                                      │
         ├─── Dispatches PUT_REQ to Node 3 ───────────────────────────────────────────────►│
         │◄── Returns STATUS_OK (Ack 2) ───────────────────────────────────────────────────┤
         │                                                                                 │
   Quorum Met (W=2)                                                                        │
   Client receives [SUCCESS]                                                               │
         │                                                                                 │
         │  ◄── Node 2 restarts ─────────────────┐                                         │
         │                                       │ (Port 9002 listening)                   │
Node 1 Gossip Thread                             │                                         │
         ├────── TCP connect(127.0.0.1:9002) ───►│                                         │
         ├────── MSG_PUT_REQ (Correlation 8888) ─►│                                         │
         │       [NODE 1] Flushed 1 hint(s)!     │ [NODE 2] Successfully applied           │
         │                                       │ delivered hint for 'cart_304'!          │
```

---

## POSIX Shell Integration

The cluster is fully integrated into a custom **POSIX C Shell** ([`POSIX Shell/`](POSIX%20Shell/)), exposing Dynamo cluster operations as native shell builtins:

```bash
# Connect to target cluster entrypoint
<svmk@Ss-MacBook-Air-2.local:~> connect 127.0.0.1 9001
[CONNECTED] Target Dynamo node set to 127.0.0.1:9001 (health check PASSED)

# Quorum Write with quoted strings
<svmk@Ss-MacBook-Air-2.local:~> put cart_101 "MacBook Pro + Studio Display"
[SUCCESS] Quorum write succeeded: 'cart_101' => 'MacBook Pro + Studio Display' (via 127.0.0.1:9001)

# Quorum Read with Unix Pipeline
<svmk@Ss-MacBook-Air-2.local:~> get cart_101 | grep "Value"
  Version 1: Value='MacBook Pro + Studio Display' | VectorClock=[(Node 1: 3)]
```

---

## Building and Testing

### Prerequisites
- Clang or GCC with C11 support
- POSIX-compliant operating system (macOS, Linux)
- `pthread` library

### Build Targets
```bash
# Compile all modules, test suites, server daemon, CLI, and Shell
make

# Compile only the cluster server daemon
make bin/dynamo_server

# Compile only the integrated POSIX Shell
make bin/dynamo_shell
```

### Running Test Suites
Every core module has a standalone unit and integration test suite compiled with `-fsanitize=address,undefined`:

```bash
./bin/test_vector_clock   # Causal dominance, concurrent siblings, pruning
./bin/test_ring           # MD5 hash ring distribution, vnodes, preference list
./bin/test_storage        # Storage engine put/get, sibling collapse
./bin/test_protocol       # Binary framing, wire serialization/deserialization
./bin/test_network        # TCP socket loopback transport
./bin/test_coordinator    # Quorum replication (N=3, W=2, R=2), read repair
./bin/test_hint           # Hint storage, capacity growth, TCP flush
./bin/test_merkle         # Merkle tree leaf insertion, key-range difference sync
./bin/test_gossip         # Heartbeat ticks, suspicion states, peer selection
```

---

## Multi-Node Cluster Walkthrough

To replicate the 3-node distributed cluster locally:

### 1. Launch 3 Nodes in Separate Terminals
```bash
# Terminal 1 (Node 1)
./bin/dynamo_server 1 9001 2:9002 3:9003

# Terminal 2 (Node 2)
./bin/dynamo_server 2 9002 1:9001 3:9003

# Terminal 3 (Node 3)
./bin/dynamo_server 3 9003 1:9001 2:9002
```

### 2. Launch the Shell Interface
```bash
./bin/dynamo_shell
```

### 3. Test Failure Injection & Hinted Handoff
1. In Terminal 2, kill Node 2 (`Ctrl+C`).
2. In `dynamo_shell`:
   ```bash
   put cart_202 "iPad Pro M4"
   ```
   *Terminal 1 logs:* `[NODE 1] Primary replica 2 is unreachable -> buffered hinted handoff`  
   *Shell outputs:* `[SUCCESS] Quorum write succeeded`
3. In Terminal 2, restart Node 2:
   ```bash
   ./bin/dynamo_server 2 9002 1:9001 3:9003
   ```
   *Terminal 1 logs:* `[NODE 1] Flushed 1 hint(s) to recovered peer 2 (127.0.0.1:9002)!`  
   *Terminal 2 logs:* `[NODE 2] Successfully applied delivered hint for key 'cart_202'!`
4. Verify consistency on Node 2 directly:
   ```bash
   connect 127.0.0.1 9002
   get cart_202
   ```

---

## Systems Defense: Key Invariants & Trade-Offs

### 1. Why Vector Clocks instead of Wall-Clock Timestamps (NTP)?
Physical clocks in distributed systems are vulnerable to clock skew, leap seconds, and NTP drift. Relying on "Last-Write-Wins" (LWW) with physical timestamps silently overwrites concurrent writes, causing permanent data loss. Vector clocks model **Lamport's happens-before relation** ($\to$), allowing the system to detect concurrency and preserve siblings for reconciliation.

### 2. Sloppy Quorum vs. Strict Quorum
In strict quorums (like Raft or Paxos), writes fail if a partition isolates the leader from a majority of primary replicas. Dynamo uses **Sloppy Quorums**: writes are accepted by the first $W$ healthy nodes in the preference list (even if they are outside the top-$N$). This guarantees write availability at the cost of transient read inconsistency.

### 3. Merkle Trees vs. Gossip for Anti-Entropy
Gossip protocols are effective for membership and liveness detection but scale poorly for data synchronization ($O(K)$ payload exchanges). Dynamo uses **Merkle trees** scoped per virtual-node key range. Nodes only exchange branch hashes; identical subtrees are pruned immediately, isolating divergent keys in $O(\log K)$ time.

---

## License
MIT License. Created for distributed systems research and educational purposes.
