# AlienFish

**Oceans of brilliants.**

AlienFish is a Stockfish derivative in honour of witty_alien. It adds a persistent opening research bank, a mode that favours searched sacrifice lines, and a mode that combines those ideas with the Alien Gambit.

Version **0.1** is a working engine and research toolkit. Stockfish's NNUE evaluation, recursive search, transposition table and tablebase code remain the basis of its play. Extra strength over Stockfish has **not** been established by a match test.

## Build

From the repository root, with GCC/Clang, Make and Python 3.10 or later:

```sh
make -C src -j4 build ARCH=native
```

The build downloads and validates Stockfish's official NNUE network. The executable is `src/stockfish` (`src/stockfish.exe` on Windows); its UCI identity is **AlienFish**. Load that executable in any UCI chess GUI.

For an optimized build using Stockfish's existing profile optimization and link optimization:

```sh
sh scripts/profile_build.sh native
```

`native` targets the build machine. Use `x86-64-avx2` for an AVX2 target, or the upstream universal build targets for broader compatibility. A native build can require instructions absent on another computer. On Windows, use an MSYS2 MinGW toolchain and `make -C src -j4 build ARCH=x86-64-avx2 COMP=mingw`, or build under WSL for Linux.

## Playing modes

Select `AlienFish Mode` in a UCI GUI, or send one of:

```text
setoption name AlienFish Mode value Normal
setoption name AlienFish Mode value Brilliant!
setoption name AlienFish Mode value Brilliant Legacy
```

| Mode | Behaviour |
|---|---|
| Normal | Stockfish's usual move selection, with optional AlienLegacy move-order advice. |
| Brilliant! | Searches several root alternatives. Among eligible moves, favours a material sacrifice visible in the principal variation, including preparation before a later sacrifice. |
| Brilliant Legacy | Chooses White's Alien Gambit opening moves when the exact position matches, then uses Brilliant! selection. |

Brilliant! requires a completed iteration, an exact score, sufficient actual root-search depth, the same tablebase rank, and a score within `Brilliant Max Loss` of the searched best move. It preserves decisive mate/tablebase choices. It does not cross from a nonnegative best score to a negative candidate score. Normal trades with an immediate recapture earn no sacrifice bonus.

The sacrifice detector checks static exchange evaluation and an accepted material investment that persists through our next move. It recognizes indirect investments, including allowing a different piece to be captured, and gives a smaller bonus to a sound offer that best defence declines. Its horizon includes later sacrifices, so a preparation move can earn a bonus. This is a **heuristic for creative play**. It is not a proof of beauty, a forced sacrifice, or a Chess.com "brilliant" classification. MultiPV costs search depth under the same clock budget; style mode can be weaker than Normal even with a small score margin.

The explicit Alien Gambit line is:

```text
1. e4 c6 2. d4 d5 3. Nc3 dxe4 4. Nxe4 Nf6
5. Ng5 h6 6. Nxf7 Kxf7 7. Nf3
```

The `3. Nd2` transposition is also recognized. Black must choose the corresponding replies; the engine cannot force its opponent into the opening. It exits the opening prefix after a deviation and honours `go searchmoves`. The prefix applies to White in standard chess, and is disabled for Chess960 and repeated positions.

**The gambit is speculative.** Brilliant Legacy deliberately selects its opening moves even when Stockfish prefers something else. Those restricted searches are excluded from AlienLegacy learning. This is the single intentional opening exception to ordinary score-based selection. See the [Chess.com opening reference](https://www.chess.com/openings/Caro-Kann-Defense-Alien-Gambit).

## AlienLegacy

AlienLegacy stores position/move records on disk and builds a bounded position index in memory. Each record contains its legal UCI move, normalized score, best searched score, actual search depth and node count. Position identity includes pieces, side to move, castling, legal en-passant state, halfmove clock and the chess variant. Fullmove numbering is ignored for transpositions. Repeated positions are neither learned nor used as advice.

Saved values are **move-order hints**. They never replace NNUE evaluation, restrict ordinary search to the book, or install an unverified exact transposition-table score. Every played move is searched again. This prevents stale or context-dependent book evaluations from being treated as current truth.

Admission requires:

1. A fully finished, unrestricted root iteration.
2. An exact centipawn score at or above the configured advantage floor, from the side-to-move's perspective.
3. A score loss from the best searched move within the configured limit.
4. Enough actual root-search depth. Bounds, incomplete iterations, mate/tablebase scores and restricted opening choices are excluded.

Deeper evidence that disqualifies a previously saved move removes that advice. Refutations survive compaction and reload, so weaker analysis cannot restore the move. Stronger new evidence can overturn a refutation. The journal has a versioned network header, per-record checksums, legal-move validation, deduplication, a memory capacity, serialized writers and compaction. Complete records survive a truncated final record. Writes occur after `bestmove` is emitted and before the worker becomes idle. Errors are reported; an incompatible file is preserved. A filesystem or power failure can still lose uncommitted writes; checksums allow recovery of complete records.

No finite-depth search can promise zero blunders. The engine enforces these measurable admission rules; deeper analysis can still overturn an earlier evaluation.

Default file: `AlienLegacy.afl`, relative to the process working directory. Set an absolute path in your GUI to make it independent of the GUI's working directory:

```text
setoption name AlienLegacy File value /absolute/path/AlienLegacy.afl
legacy status
```

A small analysed starter bank is included at `data/AlienLegacy.afl`: **101 moves across 19 positions**, with actual verification depths from 14 to 21. To use it, set `AlienLegacy File` to its absolute path. Its [metadata](data/AlienLegacy.metadata.json) records the network, admission thresholds and analysis scope. Training your own bank remains necessary for broad opening coverage.

| Option | Default | Meaning |
|---|---:|---|
| AlienLegacy | true | Use saved move-order advice. |
| AlienLegacy Learning | true | Learn eligible completed searches automatically. |
| AlienLegacy Min Depth | 12 | Minimum actual root-search depth to save/use a move. |
| AlienLegacy Min Advantage | 0 | Minimum side-to-move advantage, in UCI centipawns. |
| AlienLegacy Max Loss | 20 | Maximum loss from the best searched move, in UCI centipawns. |
| AlienLegacy Capacity | 100000 | Maximum in-memory evidence records, including refutations; supports up to 5 million. |
| Brilliant Candidates | 4 | Minimum root alternatives in style mode. |
| Brilliant Min Depth | 10 | Minimum actual depth for style reranking. |
| Brilliant Max Loss | 15 | Maximum score concession for a sacrifice line. Set to 0 for equal-score choices only. |
| Brilliant Horizon | 16 | Principal-variation plies inspected for sacrifices/preparation. |

Keep the depth defaults for play. Very low values are available for smoke tests, but produce weak evidence. To reproduce ordinary Stockfish search without any bank use or learning, disable both AlienLegacy switches and use Normal mode.

Diagnostic commands:

```text
legacy status
legacy flush
legacy compact
legacy candidates
legacy child e2e4
legacy learn e2e4 d2d4
```

`candidates` returns the last completed iteration as JSON, including actual move depths, exact/bound flags and the legal-move count. `learn` accepts an explicit list from that completed unrestricted search and reapplies all admission rules. Changing the position clears those results. `child` returns a legal child FEN/key without changing the active root.

Use one trainer per frontier database. An `.afl.lock` directory serializes journal writers. If a process is killed while holding it, stop all writers and remove the stale directory before retrying. Compaction merges completed appends before replacing the journal. Network-incompatible banks require a new bank filename or explicit reanalysis with the original network.

## Build extensive opening theory

The explorer searches **every legal move** with full MultiPV, then repeats at a greater actual depth. It expands every move that qualifies in both passes; it does not impose a top-three or top-five branch cutoff. Its SQLite frontier deduplicates positions and resumes unfinished work.

```sh
python3 scripts/train_legacy.py \
  --engine src/stockfish \
  --bank AlienLegacy.afl \
  --frontier AlienLegacy-frontier.sqlite3 \
  --side white --depth 10 --verify-depth 14 \
  --advantage 0 --max-loss 20 \
  --threads 4 --hash 512 \
  --max-positions 1000 --max-plies 24 --capacity 1000000
```

Repeat the same command to continue. `--max-positions` is the work budget for one run, not a branch-selection cutoff. Increase it for long runs. The branching tree is large: a sizeable bank needs sustained computation, and this repository does not claim that all openings have been solved.

`--side white` applies the absolute advantage floor to White's repertoire choices and explores Black's best defensive alternatives within the same loss budget. A defender can play correctly while having a negative evaluation, so those replies must still be considered. The engine's bank saves only moves that pass its own side-to-move advantage floor. Use a separate frontier with `--side black` for Black's repertoire. `--side both` strictly requires the floor for both players and can stop exploration early in positions where one side is objectively behind.

Supply `--fen` to study a specific opening position. Gambit research can begin after the sacrifice, but moves failing the bank's advantage floor will still be excluded. Frontier parameters and the network are recorded; incompatible resumes are rejected. A crash retries the interrupted node, and journal entries deduplicate.

## Verification and performance

```sh
make -C src -j4 alien-unit ARCH=native
mkdir -p /tmp/alien-unit
build/alien_unit /tmp/alien-unit
python3 tests/alienfish_test.py --engine src/stockfish -v
```

Use a fresh temporary directory for the unit test. Tests exercise storage gates, deeper refutations, corruption recovery, network mismatch, writer locks, capacity, actual sacrifices versus trades, future sacrifice preparation, score/mate guards, opening deviations, Chess960, restricted searches, interrupts, multiple threads, persistence and exploration resume.

Measure against an unmodified Stockfish executable with the same network:

```sh
python3 scripts/compare_speed.py \
  --baseline /path/to/original/stockfish --candidate src/stockfish \
  --depth 14 --runs 7 --output benchmarks/local.json
```

The script alternates engines, warms each one, uses one thread and enforces matching Normal-mode node counts. Timing varies with CPU load. Node parity supports preservation of this benchmark's search behaviour; it does not establish an Elo gain. The profile-build command is an implementation optimization whose speed must be measured on the target machine.

The initial Linux AVX2 profile build measured **2.57% higher median nodes/second** than the unmodified `-O3`/LTO baseline without PGO, across seven alternating runs per engine. Every run searched the same **2,407,430 nodes**. See the [validation report](docs/Validation.md) and [raw timing samples](benchmarks/avx2-comparison.json) for the exact comparison and limits. The policy/storage suite and all eight integration tests passed locally.

The GitHub workflow builds the engine, runs the policy/storage and UCI tests, and produces a Linux AVX2 artifact. Windows build instructions are provided; a Windows runtime result is not claimed without running it there.

## Attribution

AlienFish is distributed under **GPL-3.0-or-later**. Stockfish's copyrights, `AUTHORS`, `Copying.txt`, and source notices are preserved. The uploaded source matches official Stockfish commit `49ea5ded38315cff8e67f4a677a9e7811612fbf6` from 2026-09-30. Your original uploaded source archive is retained.

See [the upstream README](docs/Stockfish-README.md) and [official Stockfish](https://github.com/official-stockfish/Stockfish) for architecture, build targets and contributor information. This tribute is independent and does not claim endorsement by witty_alien or Stockfish.
