# AlienFish 0.1 validation

Checked on 2026-10-03, on Linux x86-64 with GCC 13.3.0 and an AVX2 build.
The uploaded baseline matches Stockfish commit
`49ea5ded38315cff8e67f4a677a9e7811612fbf6`, using `nn-252f33942263.nnue`.

## Completed checks

| Check | Result |
|---|---|
| Ordinary optimized build | Passed |
| Full profile build | Passed; all 27 source units instrumented and rebuilt with profiles, with no compiler warnings |
| C++ policy and persistence suite | Passed |
| UCI integration suite | All 8 tests passed in 4.827 seconds |
| Python compilation and shell syntax | Passed |
| Seed bank loaded by the final engine | 19 positions, 101 records, zero rejected records, no error |
| Normal-mode baseline benchmark | All 14 measured runs searched exactly 2,407,430 nodes |

The native suite checks admission thresholds, actual depth, bounds, restricted
searches, capacity, persistence, corrupt and truncated records, incompatible
networks, blocked writers, deeper refutations, and compaction. It also checks
that shallow evidence cannot resurrect a rejected move after compaction/reload,
while stronger new evidence can overturn the refutation.

Capacity regression checks cover retrying a blocked capacity change and refusing
compaction when only part of the saved bank fits in memory. Increasing capacity
then reloads every retained record and permits compaction.

Style checks distinguish accepted sacrifices, indirect queen offers, declined
offers, and future sacrifice preparation from ordinary recapture trades. They
also enforce score, depth and decisive-mate guards. UCI tests cover every White
move in the Alien Gambit prefix, the Nd2 transposition, deviations, `searchmoves`,
Chess960, Black's independent play, interrupts, terminal positions, repeated
positions, multiple threads, full legal-move analysis and explorer resume.

## Local speed measurement

The [raw report](../benchmarks/avx2-comparison.json) contains seven alternating
measurements per engine, after warm-up, at depth 14 with one thread and 16 MiB
hash. Both engines use AVX2, GCC `-O3`, LTO and the same network. The baseline is
unmodified Stockfish without PGO; the candidate adds full PGO and disables
AlienLegacy advice and learning for this comparison.

| Engine | Median nodes/second |
|---|---:|
| Stockfish baseline | 1,014,081 |
| AlienFish profile build | 1,037,238 |

The candidate's median was **2.28% higher** in this local sample. The host is
virtualized and timing varies. The result does not establish an Elo improvement,
an advantage over official Stockfish release builds, or the strength of Brilliant!
mode. Matching node counts verifies this benchmark's search signature; it is
not a proof of equivalent behaviour in every position or under every time limit.

## Opening research scope

The [starter bank metadata](../data/AlienLegacy.metadata.json) records its
provenance and SHA-256. The explorer processed 24 positions with full legal-move
coverage in two passes, and had 100 queued positions when the batch finished.
Its saved moves have actual depths from 14 to 21, side-to-move scores at least
zero, and losses no greater than 20 UCI centipawns from the searched best move.
This initial bank has limited opening coverage. The resumable explorer is the
mechanism for building a substantially larger bank.

Finite search cannot guarantee that saved moves will never be refuted. Brilliant!
selection costs MultiPV search effort, and the explicit Alien Gambit prefix is a
speculative opening choice excluded from bank learning. Long match tests and
broader opening training are still needed to measure playing strength.

The GitHub workflow builds with full profile optimization, runs the tests, and
uploads a Linux AVX2 engine. Its latest status is available in the repository's
Actions tab. Windows build instructions are supplied; a Windows runtime result
is not claimed without running it there.
