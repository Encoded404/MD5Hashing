# Comments

One thought per line. A thought is usually a sentence; keep closely related clauses of the same sentence together, and do not split at every comma. Two short sentences may share a line when they make one point. Never hard-wrap at 80 columns: break at a sentence boundary, even if the line runs long.

A colon that introduces a list normally ends the line. Each item gets its own line and its own period, and the joining "or"/"and" is dropped. A short list that fits on one continuation line may stay there. A colon that introduces an explanation may continue on the next line.

```cpp
// The solver picks a strategy:
// scan the wordlist for a single word.
// compose a phrase, when the clue contains spaces.
```

Start a new paragraph (blank comment line) when the topic changes. Copy-edit the seams ("plus" -> "and", comma before a trailing "when ..."), but keep the content: no shortening, no dropped opinions, no dashed bullets.

The same applies to `///` doc comments and `#` CMake comments.
