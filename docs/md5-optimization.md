# MD5 optimisation notes

Notes from optimising `hashlib::naive_md5`. The result is `hashlib::md5_fast`,
in `src/HashLib/hashlib.cpp`. Measured with the setup below.

## Test setup

- AMD Ryzen 9 9950X (Zen 5), Fedora 43.
- GCC 15.3.1, Clang 21.1.8, OpenSSL 3.5.8 as the reference.
- `-O3 -march=native` unless another flag is listed.
- 1 MiB payload for throughput, 55 bytes for latency, best of five runs.
- All variants were checked against OpenSSL for lengths 0 to 200, plus 255, 511,
  512, 1000, 4096, 100000 and 1 MiB.

## Throughput

| variant | gcc `-march=native` | gcc `-mtune=generic` | clang `-march=native` | 55 B native (gcc/clang) |
|---|---|---|---|---|
| `naive_md5` as shipped | 0.34 | 0.34 | 0.34 | 206 / 206 ns |
| constant tables hoisted | 0.92 | 0.87 | 0.78 | 76 / 91 ns |
| + no allocation, full blocks in place | 0.94 | 0.89 | 0.79 | 75 / 86 ns |
| + four 16-step loops | 0.96 | 1.17 | 0.93 | 66 / 67 ns |
| + fully unrolled (`md5_fast`) | 0.97 | 1.18 | 1.05 | 73 / 61 ns |
| AVX-512, scalar in vector registers | 0.55 | 0.55 | 0.68 | 137 / 112 ns |
| OpenSSL `MD5()` | 1.24 | 1.24 | 1.24 | 57 / 57 ns |

GB/s for 1 MiB; nanoseconds per call for 55 bytes. The latency column uses the
same tuning as the throughput columns; `-mtune=generic` latency is discussed
below.

The best throughput is GCC with `-mtune=generic`: 1.18 GB/s fully unrolled (1.17
with four loops), about 5% behind OpenSSL's assembly. At 55 bytes, generic
tuning reaches 54 ns with four loops and 56 ns fully unrolled, slightly ahead of
OpenSSL (57.4 ns); Clang is 60.7 ns. `md5_fast` is about 3.5x the shipped `naive_md5` at 1 MiB and
3.7x at 55 bytes.

The as-shipped row is `naive_md5` itself; the second row is the same loop with
the constant tables hoisted.

These rows predate the `Pin` barrier described under "GCC vs Clang". With it the
last row is 1.17 / 1.16 / 1.16 and the compiler columns stop differing, so read
the spread above as the history of what the source looked like at each step, not
as a property of the current implementation.

## Where the time went

### Constant tables inside the loops

`naive_md5` declares `K` inside the per-block loop and `s` inside the 64-step
loop. Both are local `constexpr` arrays; each execution of the declaration
initialises an automatic object, and GCC 15 materialises a copy of the tables
because the index is a runtime value. For a standalone GCC 15.3.1 build of the
same function that is 12 vector loads out of `.rodata` and 36 vector stores
(1.1 KiB), once per 64-byte block: a per-block cost, not a per-step one.

Hoisting both tables to namespace scope accounts for most of the gap between
`naive_md5` and `md5_fast`: 0.34 to 0.87 GB/s with GCC `-mtune=generic` (0.92
with `-march=native`), and 206 to 76 ns at 55 bytes. Hoisting changes more than
the table access: with the arrays at namespace scope GCC unrolls and
phase-splits the 64-step loop (582 instructions, 17 rotate instructions)
instead of leaving it rolled with a copy of the tables (501 instructions, 2
rotates). Most of the gain comes from that loop restructuring, not from the
copy on its own.

Clang does not copy the tables: it keeps both in `.rodata` (512 bytes) and
indexes it directly. That is a large part of why `naive_md5` is much faster
under Clang, and why the speed-up from `md5_fast` is smaller there.

### The branch chain

The remaining loop selects the round with `if (i <= 15) ... else if ...`, and
computes `g` at runtime. GCC 15 does not fully unroll it, so every step pays for
a branch and a runtime index even when the branch is predicted.

Four separate 16-step loops, or a compile-time expansion of all 64 steps,
remove the `if` chain. With each loop unrolled, `K`, `s` and `g` also become
constants, so the message load is an `add` with a constant displacement. With
`-mtune=generic`, GCC goes from 0.87 to 1.17 GB/s with four loops and 1.18
fully unrolled; Clang from 0.78 to 0.93 and 1.05.

Fully unrolling is worth about 13% on Clang (0.93 to 1.05) and nothing
measurable on GCC (0.96 to 0.97 native, 1.17 to 1.18 generic). On GCC the
four-loop form matches the full expansion; the gain is a Clang one. If the
four-loop form is kept for readability it needs `#pragma GCC unroll 16` or
`#pragma unroll`; otherwise the compiler may leave it rolled.

The expansion is an `always_inline` function template over an
`index_sequence`. A generic lambda with the same fold is not inlined by GCC;
the by-reference captures then put `a`, `b`, `c`, `d` in memory and every step
becomes a load/store round trip, 0.77 GB/s instead of 0.97.

The compressor is out of line, takes the state by value, and processes all full
blocks in one call. That keeps the state in registers between blocks and gives
one copy of the 64-step body per binary instead of one per call site.

### Allocation

Hashing full blocks directly from the caller's span instead of copying the
padded message into a `std::vector` is worth about 2% at 1 MiB. At 55 bytes the
difference is within noise (76 vs 75 ns), but removing the allocation removes a
fixed cost that matters most for the smallest messages, and it removes the
throw path.

Only the last one or two blocks need a buffer. A 128-byte stack array holds the
`0x80` byte and the 8-byte length; zero-initialising it compiles to wide stores
instead of a byte-at-a-time `push_back` loop.

### Manual rotate

`std::rotl(x, s)` and `(x << s) | (x >> (32 - s))` compile to the same
instruction on GCC 15 and Clang 21, for constant and variable counts (a `rorx`
for constants, a `rol` for variables). Throughput is unchanged: 0.97 vs
0.97 GB/s with `-march=native`, 1.18 vs 1.17 with `-mtune=generic`.

The manual form is undefined when `s == 0`, because `x >> (32 - 0)` shifts a
32-bit value by 32. MD5 never rotates by zero, but `std::rotl` is defined for
zero and there is no codegen reason to prefer the manual version.

## SIMD

There is no parallelism inside one MD5 message. Step `i+1` consumes the value
produced by step `i`, so the 64 steps are a serial chain of about five cycles
per step, roughly 300 cycles per block. That is why fast MD5 implementations
are scalar.

Computing one message in vector registers also loses on this CPU. AVX-512 can
do the round function with `vpternlogd` and the rotate with `vprold`, which
suggests a four-operation chain instead of five, but the 128-bit vector ALU
operations have about 2-cycle latency on Zen 5 while `add` and `rol` are 1
cycle:

| dependent chain | cycles per step |
|---|---|
| GPR: `F` (2 ops), `add`, `rol`, `add` | 5.0 |
| AVX-512: `vpternlogd`, `vpaddd`, `vprold`, `vpaddd` | 8.5 |

On GCC, the AVX-512 variant reaches 0.55 GB/s, less than half the scalar result,
with state, message words and constants all in XMM registers. SSE2 is worse because
it has no 32-bit rotate; AVX2 has none either.

SIMD only helps when independent messages are batched into lanes: four with
SSE2, eight with AVX2, sixteen with AVX-512. That needs a different API, not a
faster single-message `md5(span)`. The lanes supply the width; interleaving
several such groups supplies independent chains for the processor to overlap.

### `md5_batch`: one kernel per ISA, chosen at runtime

`hashlib::md5_batch` is that API. One compression covers up to `lanes`
independent messages: 4 (SSE2), 8 or 16 (AVX2), 16, 32 or 64 (AVX-512F), or 1
(scalar). The dispatcher hands over every kernel of the widest ISA and
`md5_batch` picks the narrowest one that still covers the call, so a large batch
is hashed 64 at a time on AVX-512 and 16 at a time on AVX2. Each message is
padded independently, so lengths may differ; lanes that finish early are fed
zero blocks and their digest is captured right after their own last block.

The batch subsystem is one internal module plus the implementation units around
it:

- `batch/batch.cppm` declares the kernel ABI and holds the xsimd-free 64-step
  body; it is compiled at `-march=x86-64`;
- `batch/kernel_sse2.cpp`, `batch/kernel_avx2.cpp`, `batch/kernel_avx512.cpp`
  are `module hashlib.batch;` implementation units compiled with
  `-march=x86-64`, `-mavx2`, and `-mavx2 -mavx512f` respectively;
- `batch/dispatch.cpp` is also an implementation unit, compiled at
  `-march=x86-64`. It asks the compiler for the CPU features
  (`__builtin_cpu_supports` on GCC/Clang, `_xgetbv`/`__cpuidex` intrinsics on
  MSVC) and returns a function pointer. It is deliberately baseline: if the
  dispatcher itself contained AVX, the binary would fault before it could
  choose a kernel;
- `batch/hash_many.cpp` is an implementation unit of `module hashlib`, not of
  `hashlib.batch`. It owns the per-lane padding and block scheduling for
  `md5_batch` and calls whichever kernel the dispatcher selected.

Implementation units emit no BMI, which is the point: Clang records target
features in a BMI and rejects importing an AVX2/AVX-512 BMI from baseline code,
so a module or partition compiled with those flags cannot be used by the
dispatcher. Implementation units are part of the module, are type-checked
against its interface, and are compiled with whatever per-source flags that ISA
needs. The only textual include left is third-party `xsimd` in each kernel's
global module fragment, because xsimd selects its architecture from
`__AVX2__`/`__AVX512F__` at include time.

The 64 steps are one body shared by every ISA and every group count. The step
index fixes both the role (`a`, `d`, `c`, `b`) and the message word (`j`,
`(5j + 1) % 16`, `(3j + 5) % 16`, `(7j) % 16`), so `RoleOf()` and `WordOf()` are
`constexpr`, there is no runtime index and no role-selection table, and the
state words are locals rather than references into a caller's stack frame. The
body is a fold over `std::index_sequence<64>` rather than 64 written-out calls:
that is what lets the same source compress one, two or four interleaved groups
without a second copy of the algorithm. The fold and the per-step helper are
forced inline; an out-of-line 64-step body receives the state and the schedules
by reference, which pushes every value through memory and removes the
interleaving gain entirely (measured: the outlined four-group kernel was 1.8x
slower per call than the inlined one).

This is what makes a build on an SSE2-only machine useful: the compiler can
emit AVX2/AVX-512 code without the build host supporting it, and the selection
runs on the CPU that executes the binary. The build host's ISA only matters if
`-march=native` leaks in, which is why `HASHLIB_NATIVE_TUNING` and
`HASHLIB_SIMD` are mutually exclusive.

The 16-lane kernel transposes with four 8×8 AVX2 transposes (xsimd 13's
32-bit AVX-512 transpose is a scalar scratch-buffer fallback) and joins each
pair with `vinserti64x4`. On AVX2 the 16 schedule vectors plus four state
vectors exceed the 16 `ymm` registers, so the compiler reloads or moves parts
of the schedule during the rounds; one AVX-512 group fits the whole working set
in 32 `zmm` registers. Two groups need 40 vectors and four need 80, so the
compiler spills the schedules to the stack and reloads them as memory operands.
That is cheap enough to be invisible here (a variant that forces the schedules
into memory measures the same), but it is why the four-group kernel is 2500
instructions.

#### Interleaving independent groups

A block's 64 steps are a chain: `vpternlogd`, `vpaddd`, `vprold`, `vpaddd`, each
waiting on the one before. On Zen 5 a 512-bit vector operation takes about two
cycles, so a step costs ~8 cycles while only ~5 uops issue: the vector ALUs are
less than half busy. A second independent group of messages fills the idle
slots, which is why `CompressBlock<Batch, Streams>` takes `Streams` groups per
call. The groups own disjoint lane ranges and never interact, so the digests are
identical to hashing them in separate calls.

Kernel only, one block per message, Clang 21, `-O3 -march=x86-64 -mavx2
-mavx512f`, pinned, best of five:

| kernel | ns/call | messages/call | ns/hash |
|---|---|---|---|
| AVX-512, 1 group | 136.6 | 16 | 8.54 |
| AVX-512, 2 groups | 138.0 | 32 | 4.31 |
| AVX-512, 4 groups | 205.1 | 64 | 3.20 |
| AVX2, 1 group | 115.0 | 8 | 14.39 |
| AVX2, 2 groups | 116.0 | 16 | 7.23 |

Two groups cost about 1% more wall time than one and do twice the work. Four
groups are past the point where the chain matters and are limited by the vector
ALUs instead, which still buys another 34%. The two-group AVX2 kernel reaches
the one-group AVX-512 throughput, which is the same effect from the other
direction: the number of independent chains matters, not the lane width.

Measured on an i5-1250P (AVX2, no AVX-512), Clang 21 + libc++, baseline
`-march=x86-64` build, 43-byte text. These predate the interleaved kernels: the
8-message row still selects the single-group kernel, while the larger batches
now use two groups:

| path | ns/call | ns/hash | GB/s |
|---|---|---|---|
| `md5_fast` (single) | 82.2 | 82.2 | 0.52 |
| `md5_batch`, 8 messages/call | 143.8 | 18.0 | 2.39 |
| `md5_batch`, 16 messages/call | 278.7 | 17.4 | 2.47 |
| `md5_batch`, 64 messages/call | 1096.4 | 17.1 | 2.51 |

At 4 KiB per message the setup cost disappears and the kernel dominates:

| path | ns/hash | GB/s |
|---|---|---|
| `md5_fast` | 5104.0 | 0.80 |
| `md5_batch`, 16 messages/call | 824.7 | 4.97 |
| `md5_batch`, 64 messages/call | 818.7 | 5.00 |

So the batch path is about 4.7× at 43 bytes and 6.2× at 4 KiB on AVX2. The
remaining gap to the 8-lane ideal is the per-block schedule transpose, the
scalar padding of every lane, and AVX2's three-instruction rotate. AVX-512
uses one `vprold`, `vpternlogd` for F, and twice the lanes; it compiles to
`zmm`/`vprold`/`vpternlogd` (verified with `objdump`) but was not run here
because this CPU has no AVX-512. `HASHLIB_FORCE_ISA=sse2|avx2|avx512f|scalar`
exercises each available kernel on one machine; Intel SDE or a Zen 4/5 runner
is needed to execute the AVX-512 path.

Re-measured on the Zen 5 machine (AVX-512), `HashApp -b --benchmark-batch 16`
(which selects the single-group AVX-512 kernel), `md5_batch` against `md5_fast`
on the same build:

| message | `md5_fast` GB/s | `md5_batch` GB/s | speed-up |
|---|---|---|---|
| 43 B | 0.69 | 3.66 | 5.3x |
| 1 KiB | 1.02 | 6.79 | 6.7x |
| 4 KiB | 1.07 | 7.27 | 6.8x |
| 64 KiB | 1.09 | 7.39 | 6.8x |
| 100 KB | 1.08 | 7.40 | 6.9x |

Forced kernels at 4 KiB, same batch size: sse2 (4 lanes) 1.52, avx2 (16 lanes,
two groups) 6.84, avx512f (16 lanes, one group) 7.38, scalar (1 lane) 1.07 GB/s.
The two-group AVX2 kernel lands within 8% of the one-group AVX-512 kernel, which
is the same observation from the other direction: the number of independent
chains sets the time per call, not the lane width.

With the interleaved kernels the same `HashApp -b` run, by batch size:

| messages/call | kernel | 43 B ns/hash | 4 KiB ns/hash |
|---|---|---|---|
| 16 | AVX-512, 1 group | 11.4 | 562 |
| 32 | AVX-512, 2 groups | 6.7 | 288 |
| 64 | AVX-512, 4 groups | 5.1 | 205 |

At 43 B the per-message padding and bookkeeping do not shrink with the kernel,
so the call gains 1.7x at 32 messages and 2.2x at 64; at 4 KiB the kernel
dominates and the four-group kernel is 2.7x the one-group one. Batches larger
than 64 keep the same per-message cost, because they simply take more calls.

### Per-message bookkeeping in `md5_batch`

At one block per message the four-group kernel spends 3.2 ns per message, and
the wrapper around it was spending about as much again. Two properties of the
caller decide what can be removed. Callers that group their messages by length,
as the phrase solver does, hand over batches where every span has the same size.
And `md5_batch` is the only place that pads, so it is also the only place that
knows how much of the work repeats per lane.

The wrapper therefore takes a fast path when a batch is equal-length. One tail
block is built once and copied into every lane with a constant-size move, the
per-lane `memcpy` of the message prefix becomes a fixed-size overlapping copy
instead of a call to the C library, the initial state is written with four
constant vector fills instead of four stores per lane, every lane finishes on
the same block so the per-lane block choice and per-lane digest test disappear,
and the digests are packed in one pass at the end. Unequal lengths keep the old
per-lane path unchanged.

`utilities::PackValues` also moved into the module interface. It is four moves,
and as an implementation-unit function it could never be inlined, so every
message paid a call to pack its digest.

Measured on the Zen 5 machine, 21-byte messages, `HashApp -b
--benchmark-batch 64`, best of five:

| version | ns/call | ns/hash |
|---|---|---|
| per-lane padding | 390.8 | 6.1 |
| equal-length fast path | 300.0 | 4.7 |

That is 1.30x on the call, and it leaves 1.5 ns per message of bookkeeping
against 3.2 ns of compression. In the phrase solver the `hash` phase went from
47.7 ms to 36.0 ms on the 7,260,272-candidate search, and the solve total from
101 ms to 88 ms.

A step is `vpternlogd`, `vpaddd`, `vprold`, `vpaddd`, and Zen 5 vector ALU
operations measure 2 cycles each (the GPR `add`/`rol` path is 1), so one step is
8 cycles and one block of 16 messages is 512 cycles: 2 bytes per cycle, or
11 GB/s at 5.5 GHz. That is a latency bound, not a throughput bound — the ALUs
are less than half busy — which is exactly why the extra groups work. 7.29 GB/s
was 1.32 bytes per cycle, about 66% of the one-group bound; most of the missing
third is the per-block transpose, and the rest is the per-lane pointer and digest
bookkeeping in `hash_many.cpp`, which scale with lanes and blocks rather than
with bytes.

## Compiler flags

`-march=native` implies `-mtune=znver5` on GCC, and GCC's znver5 model produces
a worse schedule for this kernel: 0.97 GB/s instead of 1.18 with
`-mtune=generic`, or 5.7 vs 4.8 cycles per step on the loop-carried chain.
Instruction selection is not the cause; disabling BMI2 does not recover it.
Clang behaves the other way round: 1.05 GB/s with native tuning, 0.97 with
generic.

`-O2` and `-O3` are within noise, `-flto` changes nothing, and
`-march=x86-64-v2` is several percent slower than `-march=native
-mtune=generic`, mostly through tuning.

If `-march=native` is used in this project, pin `-mtune=generic` for the hash
target and re-measure after compiler upgrades. Note that this section is
historical for `md5_fast`: the `Pin` barrier described under "GCC vs Clang"
removed the tuning sensitivity, and every compiler now lands on the same
schedule. It still applies to `naive_md5` and to code that has not adopted the
barrier.

The `HASHLIB_NATIVE_TUNING` CMake option is the supported way to use it. It
still buys about 5% on the single-message path — 1.10 GB/s to 1.15 GB/s in the
shipped configuration, on both compilers — but only with `HASHLIB_SIMD=OFF`,
because `-march=native` would make the runtime dispatcher non-portable. For bulk
hashing the batch kernels are worth 6x, so the tuning option is the wrong trade
unless the workload is a single message at a time.

The option has to apply to every translation unit that imports the module, not
just to the library. A Clang module BMI records the target features it was built
with, and importing it from a TU with a different feature set is a hard error
(`precompiled file ... was compiled with the target feature '+avx512f' but the
current translation unit is not`), so `src/HashLib/CMakeLists.txt` attaches the
flags to an `INTERFACE` target linked `PUBLIC` and the options reach `HashApp`
and the tests through the library's usage requirements.

## GCC vs Clang

The same `md5_fast` source, four builds, 1 MiB payload:

| build | naive | four loops | fully unrolled | 55 B (unrolled) |
|---|---|---|---|---|
| gcc `-mtune=generic` | 0.87 | 1.16 | **1.16** | 57 ns |
| gcc `-march=native` | 0.91 | 0.96 | 0.96 | 67 ns |
| clang `-march=native` | 0.77 | 0.93 | 1.04 | 61 ns |
| clang `-mtune=generic` | 0.78 | 0.79 | 0.96 | 66 ns |

Best to worst is a ratio of 1.22, and it is not "Clang is faster" or "GCC is faster": each
compiler has a machine model that happens to be good or bad for this kernel.
GCC's `znver5` (what `-march=native` implies) costs 17% against
`-mtune=generic`; Clang's `generic` costs 8% against its native `znver5`.

Disabling the schedulers does not move anything (GCC 1.16 with
`-fno-schedule-insns2`, Clang 1.04 with `-mllvm -enable-misched=false`), and
neither does loop alignment. It is a code-shape difference, and it comes down to
one thing: **how many cycles of dependent work sit between the round function
and the rotate**.

### Zen 5 dependent-op latencies

Measured with a chain of the same instruction, 10 per iteration, against the
5.51 GHz the same harness reports for a dependent `add`:

| op | cycles |
|---|---|
| `add`, `rol r,imm`, `rorx r,imm` | 1 |
| `lea` with base + index | 1 |
| `lea` with base + index + displacement | **2** |

The 3-component `lea` is the one that matters. Every implementation wants to
compute `a + X[k] + K[i]` in one instruction, and that is exactly the 2-cycle
form. It is free when the operand is the old `a`, and it costs a cycle when the
operand is the value the previous step just produced.

### The four shapes

GCC `-mtune=generic`, 1.16 GB/s. The `lea` runs off the critical path and a
single `add` joins it to the round function:

```
lea  -0x28955b88(%r12,%rdi,1),%edx   ; a + X[0] + K[0], a is three steps old
add  %edx,%eax                       ; + F
rorx $0x19,%eax,%eax
add  %r8d,%eax                       ; + b
```

GCC `-mtune=znver5`, 0.95 GB/s. It knows the 3-component `lea` is 2 cycles, so
it rearranges to give the `lea` fewer inputs — and puts it after the `add`,
which now depends on `F`, which depends on the fresh `a`:

```
add  %r12d,%eax                      ; F + a      <- on the chain
lea  -0x28955b88(%rsi,%rax,1),%ecx   ; + X + K    <- 2 cycles, on the chain
rorx $0x19,%ecx,%ecx
add  %edi,%ecx
```

Clang `-march=native`, 1.04 GB/s, one `lea` but on the chain:

```
add  %esi,%ecx
lea  -0x28955b88(%rax,%rcx,1),%eax   ; 2 cycles, index is the fresh value
rorx $0x19,%eax,%eax
add  -0x78(%rsp),%eax
```

Clang `-mtune=generic`, 0.96 GB/s, no folding at all — and 254 `add`s per block
against 133 for GCC:

```
add  %esi,%ecx
lea  (%rbx,%rcx,1),%eax              ; + X
add  $0xd76aa478,%eax                ; + K, second dependent op
rorx $0x19,%eax,%eax
add  -0x78(%rsp),%eax
```

So the ranking follows the chain length exactly: 1 op after `F` (1.16) beats one
2-cycle `lea` after `F` (1.04) beats two dependent ops (0.96) beats an `add` plus
a 2-cycle `lea` (0.95).

The 4.75 cycles per step that 1.16 GB/s works out to at 5.51 GHz is the
structural floor: rounds 1, 2 and 4 need two operations on the accumulator
before the rotate and round 3 needs one, `(16*5 + 16*5 + 16*4 + 16*5) / 64 =
4.75`.

### The `Pin` barrier

The four shapes above are what one source line produces under different tunings.
`md5_fast` now blocks the reassociation with an empty inline-asm barrier:

```cpp
const std::uint32_t message = Pin(k + LoadLe32(block + 4 * g));
target = std::rotl((target + message) + f, shift) + x;
```

`Pin()` is `asm volatile("" : "+r"(value))` on GCC and Clang, and the identity on
MSVC. It emits no instructions; it stops the optimiser sinking `k` or `X[g]`
behind `f`, which is what puts a third dependent operation on the rotate's
chain.

Re-measured, 1 MiB, same harness, same machine:

| build | before | after | |
|---|---|---|---|
| gcc `-march=native` (znver5) | 0.963 | 1.166 | 1.21x |
| gcc `-mtune=generic` | 1.167 | 1.160 | 1.00x |
| clang `-march=native` | 1.048 | 1.163 | 1.11x |
| clang `-mtune=generic` | 0.964 | 1.164 | 1.21x |
| clang `-march=x86-64` (shipped build) | 0.921 | 1.106 | 1.20x |

The compiler and tuning spread is gone: every `-march=native` configuration now
lands at 1.16 GB/s, 4.75 cycles per step, which is the structural floor. Two of
the conclusions above are therefore historical: `-mtune=generic` is no longer
needed for `md5_fast`, and the four "shapes" are now one shape on every compiler.
`-march=native` is still worth 5% over the baseline ISA (1.106 to 1.163) because
it enables `rorx`, `andn` and BMI2, but the *tuning* half of the flag no longer
matters.

One measurement trap comes with it: because the barrier is `volatile`, the hash
is no longer a pure expression to the optimiser, so the old and new code cannot
be compared by calling them in a loop with an invariant input — GCC hoists the
old version out of the loop and reports 0.6 ns for a 1 MiB call. Use an opaque
function pointer, as the A/B harness in this repository does.

### Clang's spilling

Clang spills about 30 values per block — 60 stack references inside the loop body
against 4 to 6 for GCC:

```
mov  %rdx,-0x18(%rsp)
mov  %r10,-0x10(%rsp)
mov  %rdi,-0x20(%rsp)
and  -0x78(%rsp),%ecx
```

The spills are mostly message words loaded early for later steps, so they sit off
the critical path and cost far less than the count suggests — but they do not
help. The 64-bit stores (`mov %rsi,-0x58(%rsp)` for a 32-bit word) are the
allocator parking the value rather than rematerialising the load.

## Against OpenSSL

OpenSSL's `md5_block_asm_data_order` is the perlasm implementation, reachable
through `MD5_Transform`. Measured in the same process, alternating between the
two implementations so both see the same clock:

| | 1 KiB | 64 KiB | 1 MiB |
|---|---|---|---|
| `md5_fast` (gcc `-mtune=generic`) | 1.080 | 1.162 | 1.166 |
| OpenSSL `MD5()` | 1.131 | 1.227 | 1.231 |
| ratio | 1.047 | 1.056 | 1.056 |

`bench/md5_ab.cpp` reproduces this. The ratio is flat from 64 KiB up, so it is a
per-step cost and not a memory effect; 1 KiB is slightly closer only because
`MD5()` pays more per-call setup, which flatters the comparison for us. Coarse
interleaving (64 MiB per switch) gives the same 1.056 as 1 MiB per switch, so
this is cycles and not clock. The gap is 5.6%, or about 0.3 cycles per step.

The surprising part is the instruction count:

| | instructions per block | per step | stack refs in loop |
|---|---|---|---|
| gcc `-mtune=generic` | 554 | 8.66 | 4 |
| clang `-march=native` | 564 | 8.81 | 60 |
| clang `-mtune=generic` | 623 | 9.73 | 61 |
| OpenSSL | 610 | **9.53** | 0 |

OpenSSL emits 10% *more* instructions per step than GCC and is 5.6% faster. It is
not minimizing work, it is minimizing depth, and the extra instructions are the
ones that keep old values available:

- Every message word is loaded immediately before the step that uses it — 64
  loads, at most 10 instructions ahead of the first use, median 7. GCC hoists
  loads up to 103 instructions early (median 7, mean 10), so a word can be live
  long before the round that consumes it. That is where Clang's 60 stack
  references come from; GCC avoids them by having more registers to park things
  in, not by being cleverer.
- Roles are rotated by emitting the unrolled text with different registers per
  step, with no `mov` for the rotation.

Neither compiler is doing anything stupid; both are optimising instruction count
and register pressure, which is the right default and the wrong objective for a
64-step serial chain.

The chain itself is identical — `lea`, `add F`, `rol`, `add b` — so the 5.6% is
not a shorter recurrence; it is where the critical operation lands in the
cycle-by-cycle schedule. That is also why 1.16 GB/s is
close enough to 1.23 to stop: a compiler reaching 95% of perlasm on a 64-step
serial chain is unusual, and the remaining 5.6% is not available from the C++
side without hand-written assembly.

## Smaller findings

- `__builtin_prefetch(p + 64)` before each block changes nothing: no change at
  either tuning (0.97 and 1.18 GB/s). The hardware prefetcher already handles
  the sequential stream.
- A streaming `update`/`final` API measures the same as the one-shot version as
  long as full blocks are consumed in place; only the trailing partial block is
  buffered. Both can be offered without a throughput cost.
- Rounds 1 and 2 of `md5_fast` use the ternary forms (`d ^ (b & (c ^ d))` and
  `c ^ (d & (b ^ c))`) instead of the original and/or/not forms, for example
  `(b & c) | (~b & d)` in round 1. They compile to the same number of
  instructions but keep the complement off the dependency chain. Round 4 has no
  cheaper equivalent.
- The `LOGIFACE_LOG(trace, ...)` call in `naive_md5` is guarded and costs
  nothing at the default level, but at trace level every hash does two or three
  allocations and a timestamp. Moving the log to the call site avoids changing
  the performance profile when trace logging is enabled.
- `std::bit_cast` on the block and on the length assumes little-endian. On a
  big-endian target both are byte-swapped and the digest is wrong. MD5 is
  defined little-endian, so the load and store helpers must be explicit; a
  4-byte `memcpy` compiles to one unaligned move on x86-64 and ARM64.

## Correctness notes on `naive_md5`

- See the endianness note above.
- `std::uint16_t bitsToAppend = (447 - lengthInBits) % 512;` relies on unsigned
  wraparound before narrowing. It produces the right value for every length
  tested, but reads like an underflow. `(56 - len % 64) % 64` or the
  `(rest + 9 <= 64) ? 1 : 2` block-count form is clearer.
- The per-block `std::array<std::byte, 64> block{}` zeroes 64 bytes and then
  overwrites all of them with `memcpy`.
- `md5_fast` is `noexcept`; `naive_md5` cannot be while it keeps the vector and
  the trace log.
- The `std::uint16_t` to `std::size_t` block-counter fix in `110ba9f` is correct
  and covered by `tests/hashlib_tests.cpp`. No other length arithmetic is
  size-limited.

## Implementation

`hashlib::md5_fast` is in `src/HashLib/hashlib.cpp`. It passes the RFC 1321
vectors and the padding-edge tests, and agrees with `naive_md5` for every length
from 0 to 200.
