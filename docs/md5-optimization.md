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

## Where the time went

### Constant tables inside the loops

`naive_md5` declares `K` inside the per-block loop and `s` inside the 64-step
loop. Both are local `constexpr` arrays; each execution of the declaration
initialises an automatic object, and GCC 15 materialises a stack copy because
the index is a runtime value. `K` costs 256 bytes once per block, `s` costs 256
bytes 64 times per block: about 16 KiB of stores per 64-byte block.

Hoisting both tables to namespace scope accounts for most of the gap between
`naive_md5` and `md5_fast`: 0.34 to 0.87 GB/s with GCC `-mtune=generic` (0.92
with `-march=native`), and 206 to 76 ns at 55 bytes.

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
faster single-message `md5(span)`.

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
target and re-measure after compiler upgrades.

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
