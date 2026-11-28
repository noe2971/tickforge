Engine `reference`, mode `throughput`: 368366634 messages, 363118215 book messages, 0 book calls returned an error; hash of every book at 11 points of the day (each 1 GiB chunk boundary and the end) `6dd14ebb3f3cea3b`.

<!-- digest=6dd14ebb3f3cea3b -->
| Pass | Book s | M msg/s (all) | M book msg/s | ns per book msg | Inflate s | Wall s | Heap allocations in book segments |
|---|---:|---:|---:|---:|---:|---:|---:|
| 1 | 91.61 | 4.0 | 4.0 | 252.3 | 10.40 | 102.68 | 442865912 |
| 2 | 91.38 | 4.0 | 4.0 | 251.6 | 10.50 | 102.54 | 442865912 |
| 3 | 92.54 | 4.0 | 3.9 | 254.9 | 10.51 | 103.73 | 442865912 |
| **median** | 91.61 | 4.0 | 4.0 | 252.3 | 10.40 | 102.68 | 442865912 |
<!-- median_book_s=91.606237 -->
