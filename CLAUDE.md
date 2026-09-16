# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project status

This is a university (PCD - Programação Concorrente e Distribuída) project. No implementation code exists
yet — only the directory skeleton (this section) and the technical specification
(`docs/especificacao_tecnica.pdf`) that defines the entire project design. Read that spec before implementing
anything; it is the single source of truth for data structures, parallelization strategy, and the experimental
protocol described below. `docs/estrutura_projeto.md` explains the rationale behind the directory layout.

## What this project is

A comparative study of parallel vector similarity search (nearest neighbor search) using OpenMP:

- **Brute-force**: exact, exhaustive search — computationally expensive but easy to parallelize uniformly.
- **LSH (Locality-Sensitive Hashing)**: approximate search — faster per query but with recall loss.

Research question: how does thread-based parallelism affect performance and energy efficiency for each
approach, and at what thread count does brute-force become competitive with LSH?

## Tech stack

- **Language**: C++
- **Parallelism**: OpenMP (`#pragma omp parallel for`)
- **Compiler**: `g++` with `-fopenmp` (e.g. `g++ -O2 -fopenmp -o search main.cpp`)
- **Dataset**: SIFT1M from ann-benchmarks — 1M vectors, 128 dimensions, `.fvecs` format (fall back to
  SIFT100K/SIFT10K if hardware can't handle the full set)
- **Test environments**: Google Colab for development, university lab for final timing/energy measurements
- No vector database (Qdrant, FAISS, Pinecone, Milvus, OpenSearch) is used in the experimental core — none of
  them expose per-search thread control (`num_threads(N)`) as required by the protocol, and all add
  network/serialization overhead that would contaminate measurements. The "database" is a custom in-memory
  structure.

## Repository layout

```
database/           .fvecs/.ivecs parser + VectorDataset loading/population ("database" is in-memory only,
                    not a persistent service — see docs/estrutura_projeto.md)
common/             code shared by both algorithms: VectorDataset struct, distance function, top-K/merge
                    utilities, CSV metrics writer
scripts/
  brute-force/      exact search (sequential + OpenMP-parallel), per docs/especificacao_tecnica.pdf
  lsh/              approximate search: Bucket/LSHIndex, hashing, parallel indexing + batched query search
data/               raw dataset files (.fvecs/.ivecs) — gitignored, not committed
results/            CSV output of experiment runs — gitignored, not committed
Makefile            root orchestrator: `make database`, `make populate`, `make brute-force THREADS=N`,
                    `make lsh THREADS=N` (targets are currently TODO placeholders, no logic yet)
```

`THREADS` is a parameter on **both** `brute-force` and `lsh` Makefile targets, not just brute-force — the
experimental protocol compares both algorithms across the same thread counts. Shared logic (the
`VectorDataset` struct, distance function, top-K merge, metrics/CSV writer) belongs in `common/`, not
duplicated inside `scripts/brute-force/` or `scripts/lsh/`. Algorithm-specific structures (`Bucket`,
`LSHIndex`) stay inside `scripts/lsh/`, not in `common/`.

## Core data structures (fixed by the spec — do not redesign)

```cpp
struct VectorDataset {
    std::vector<float> data; // flattened row-major: N * D
    int n; // number of vectors
    int d; // dimension (128 for SIFT1M)
};

struct Bucket {
    std::vector<int> vector_ids;
};

struct LSHIndex {
    std::vector<Bucket> buckets;
    int num_buckets;
};
```

Vectors are stored as a single flat `std::vector<float>` (row-major, N*D floats), **not** as a
`vector<vector<float>>`. Vector `i`, dimension `j` is `data[i * D + j]`. This is deliberate for cache locality
and single-allocation access under concurrent threads — do not switch to nested vectors.

`.fvecs`/`.ivecs` format: each vector is stored as `[int32 dimension][d x float32 values]`, repeated per
vector. The per-vector dimension prefix must be read/verified and discarded, not appended to the data array.
The same parser works for the dataset, `sift_query.fvecs`, and `sift_groundtruth.ivecs` (the latter uses ints
instead of floats).

## Parallelization design (fixed by the spec)

**Brute-force**: work is divided by vector index range first, then translated to memory offsets — no thread
ever splits a vector across a boundary. Each thread scans its contiguous slice of the dataset against the
query, keeps a local top-K (heap or partial sort), and a fast sequential merge step reduces all local top-K
lists into the global top-K. This merge is the "reduction" step referenced in the experimental protocol.
Brute-force has no indexing phase — measured time is pure search time.

**LSH**: has two distinct parallel phases that scale differently:
- *Indexing* (once, before searches): each thread hashes a range of vectors into buckets. Since vectors from
  different ranges can land in the same bucket, writes need either a per-bucket lock or thread-local
  accumulation merged into buckets at the end.
- *Search*: each query only touches the vectors in its bucket(s), not the whole dataset, so per-query work is
  small — parallelizing within a single query search yields little. Thread gains come from processing many
  queries concurrently (one thread per query batch) or from the indexing phase.

This asymmetry (brute-force: large uniform work, trivial to divide; LSH: small per-query work, cost
concentrated in indexing) is central to the paper's hypothesis and must be reflected in how each is
benchmarked — LSH indexing time is measured and reported separately, never summed into the search-time
comparison against brute-force.

## Implementation order (from the spec's Etapas)

1. `.fvecs`/`.ivecs` parser; load dataset/queries/ground truth into `VectorDataset`; validate N, D, and sample
   values against the raw file.
2. Sequential brute-force (Euclidean distance, top-K via `std::priority_queue` or partial sort). Validate
   recall == 1.0 against SIFT1M ground truth before moving on.
3. Parallel brute-force with OpenMP: per-thread local top-K over its slice, then sequential merge. Benchmark
   at 1, 2, 4, 8... threads.
4. Sequential LSH (random projection hash families, bucket construction, bucket-restricted search). Recall is
   expected to be < 1.0 against ground truth.
5. Parallel LSH: parallelize bucket construction (indexing) and multi-query search, using the same thread
   counts as step 3.
6. Experimental protocol: fixed seeds/params across configs, multiple runs averaged per (algorithm × thread
   count), metrics collected to CSV (time, speedup, parallel efficiency, recall, energy/CPU usage).
7. Analysis: time-vs-threads, speedup-vs-threads, recall-vs-algorithm plots; identify the thread count where
   parallel brute-force matches/exceeds LSH; discuss energy results against the hypothesis; write up the
   article (ABNT references already listed in the spec).

## Metrics that must be collected

Time per thread configuration (1, 2, 4, 8, ...), speedup, parallel efficiency, energy consumption (direct
measurement if available, otherwise CPU usage as a proxy), and recall (LSH neighbors found vs. brute-force
ground truth).
