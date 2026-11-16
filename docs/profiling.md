# Profiling terrain generation

Where a cold start's time goes, how that was measured, what was done about
it and what is left. Written so a later session can carry on without
repeating the measurements that settled each point.

There are no timings here, on purpose: this machine's runs vary by a fifth
with whatever else it is doing, so a figure written down is wrong by the time
it is read. Counts are reproducible and are given. Measure the times again
when they matter; the tools are below.

## How to measure

- **The settle log.** Each time the octree settles it logs one line per LOD:
  how that LOD's jobs ended (skipped, probed empty, sampled empty, meshed),
  the workers' milliseconds in total, then split into the probe's macro and
  detail, the chunk's macro, detail and coarse output, and the climate, each
  macro figure followed by its graph evaluations in thousands. The first
  settle of `encke --scenario scenarios\pole.json` is the cold start at the
  test floor. It is reproducible in its counts: the same jobs end the same
  way every run.
- **`encke --headless`** prints the detail octaves per LOD and what each
  macro graph costs against one Simplex evaluation, then times LODs 0 and 4
  on one thread.
- **`encke --sweep [n] [--threads t]`** times a chunk at every LOD and prints
  a hash of every output bit. The hash is the check that an optimisation
  changed nothing; the `ours%` column is the share of a chunk's noise time
  that is this project's code and not FastNoise2.
- Measure with the `release` preset. Its exe needs `F:\msys2\ucrt64\bin` on
  PATH or it exits with `0xC0000135`.
- Compare runs by per-LOD worker time and evaluation counts, not by the time
  to settle. Job times are wall-clock inside the job, so a busy machine
  inflates all of them together, and the LODs a change did not touch show by
  how much.
- Keep `cache/` between runs being compared. A run after `--clear-cache`
  bakes the surface map, which takes workers from the chunks.

### Going easy on the machine

This is a 4-core, 8-thread desktop the user works on, and an all-core run
has taken it to 96°C.

- `--sweep` runs its 200-rep FastNoise2 floors for all 21 LODs whatever chunk
  count it is given, so `--sweep 1` is not a quick run, least of all under
  clang-sanitize. `--sweep 8 --threads 2` on release is enough for the hash
  and a usable timing table. Run it once per side of a change.
- `$env:CMAKE_BUILD_PARALLEL_LEVEL = "4"` before `.claude\cmake.ps1` halves a
  build's load.
- A change to a `PUBLIC` compile definition in `CMakeLists.txt` rebuilds
  every source in every preset, and re-runs `vcpkg install`.

## The shape of a cold start

The worker pool is one thread per physical core less one: three here. The
workers are saturated from the first frame to the settle, and the time to
settle is their summed job time over three. Nothing else is in the way: not
the queue, not the main thread's drain.

At the test floor the first settle is 4,343 jobs, about 180 to 290 at every
LOD from 0 to 18. The roots, LOD 20, all split; LOD 19 has 19.

| LOD | Probed empty | Sampled empty | Meshed |
| --- | --- | --- | --- |
| 0 | 94 | 93 | 97 |
| 1 | 67 | 84 | 68 |
| 2 | 22 | 86 | 94 |
| 3 | 50 | 103 | 97 |
| 4 | 66 | 90 | 91 |
| 5 | 58 | 79 | 82 |
| 6 | 67 | 94 | 94 |
| 7 | 54 | 75 | 82 |
| 8 | 55 | 70 | 82 |
| 9 | 38 | 74 | 66 |
| 10 | 76 | 69 | 86 |
| 11 | 59 | 65 | 65 |
| 12 | 94 | 77 | 95 |
| 13 | 50 | 95 | 67 |
| 14 | 41 | 92 | 66 |
| 15 | 70 | 72 | 82 |
| 16 | 65 | 98 | 100 |
| 17 | 47 | 81 | 103 |
| 18 | 24 | 110 | 103 |
| 19 | 0 | 4 | 15 |
| all | 1,097 | 1,611 | 1,635 |

What a job evaluates:

- Detail octaves: 10 at LOD 0, one fewer per LOD down to 2 at LOD 8, 1 at
  LODs 9 and 10, none from LOD 11. Octaves 10 and 11 of the configured 12 are
  never reached with the finest LOD at 0.
- Macro lattice: 64 m apart below LOD 8, so a fine chunk spans a few nodes
  and its macro cost is nothing. From LOD 8 the lattice is the voxel grid:
  37³ nodes for a chunk's 36³ samples, one graph evaluation per node per
  channel read.
- The probe is on its own stride's lattice, 11³ nodes from LOD 6 up.
- The climate, read at a meshed chunk's vertices, lays out the lattice box
  around all of them: up to the chunk's own node count, for two graphs.
- The graphs, counted in the code: height is 14 Simplex evaluations and one
  domain warp, which is a single 8-corner lattice lookup; detail amplitude 6,
  ridge 3, persistence 2, temperature 5, moisture 9. Timed, height costs more
  than the other three field graphs together: its power, remaps and fractal
  nodes add to the count.

Where the time is:

- Fine chunks, LOD 0 to 7, are the detail octaves. By the sweep's `ours%`
  column a large part of that is this project's code around FastNoise2: the
  block copies in `box_octave` and the accumulation and composition in
  `combine`.
- Coarse chunks, LOD 8 up, are the macro graphs.
- Across every LOD, half of the chunks sampled in full then mesh nothing.
  That is the largest single waste left.
- The surface map's bake was about a fifth of the workers' time before it
  was cached.

## Done

- **The surface map is cached on disk** (`docs/terrain.md`, commit
  `612dece`). The bake no longer competes with the chunks after the first
  run.
- **A chunk samples only the macro channels its octaves read** (commit
  `abbcd92`): height alone from LOD 11, no persistence at LODs 9 and 10.
  Graph evaluations from LOD 11 up fell to a quarter, the time by less, since
  height is the costly graph. The sweep hash, `3b07b1f5be32bca9` at
  `--sweep 8`, did not move.

## Ruled out

- **Compacting the probe's lattice axes.** The idea was that the probe lays
  out the chunk's whole lattice, 38³ nodes, to read 1,000 points. It does
  not: it lays out 11³, all of them used. Nothing to remove.
- **The domain warp as the height graph's cost.** It is one lattice lookup.

## Open, in the order worth trying

1. **A tighter probe.** `certainly_empty` assumes a slope of 5 everywhere,
   which is LOD 0's with ten octaves. On its 4-voxel lattice that is a reach
   of 17 voxels in a 32-voxel chunk, so nearly anything close to the surface
   is sampled in full: the 1,611 above. Coarser LODs are far gentler, about
   1.9 at LOD 4. Candidates: a slope bound per LOD, measured by `planet_test`
   as the present one is, and a second pass 2 voxels apart for chunks the
   first leaves undecided. Size it before building it: log each sampled-empty
   chunk's smallest |value| in voxels for one cold start, which shows how
   many a given reach would have caught. A bound too tight makes holes.
2. **The detail path's own overhead** at LOD 0 to 7. Start from the sweep's
   `ours%` and find which of `box_octave`'s copies and `combine`'s loops it
   is. Every sample must keep the same operations in the same order; the
   sweep hash is the check.
3. **More workers.** Three threads leave the SMT siblings idle. The workers
   run at background priority, so frames are protected. AVX2 code gains less
   from SMT than most, and it will run the CPU hotter. Unmeasured; a one-line
   experiment in how `App` sizes the pool.
4. **The climate's lattice box.** `MacroField::sample` evaluates every node
   between the lowest and highest cell its points touch, and a chunk's
   vertices touch only the cells near the surface. Compacting each axis to
   the cells used, the idea ruled out for the probe, fits here. A few percent.

Larger, and the user's call before any work:

- **Caching chunk meshes on disk.** A repeat start at the same pose, the
  scenarios' and the development loop's case, would cost almost nothing. A
  new place gains nothing, and a stale mesh is harder to catch than a stale
  map.
- **Deferring chunks below the horizon.** From eye height most of LOD 10 and
  up is hidden. It changes what settled means, and touches shadows and the
  swap rule.

## What flying shows

Flying fast, the churn is coarse chunks swapping, so the macro change shows
there more than in the first load, which has every LOD at once and is still
the longest wait. The first two open items are the ones that shorten it.
