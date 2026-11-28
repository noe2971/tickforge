Engine `fast`, mode `throughput`: 368366634 messages, 363118215 book messages, 0 book calls returned an error; hash of every book at 11 points of the day (each 1 GiB chunk boundary and the end) `6dd14ebb3f3cea3b`.

<!-- digest=6dd14ebb3f3cea3b -->
| Pass | Book s | M msg/s (all) | M book msg/s | ns per book msg | Inflate s | Wall s | Heap allocations in book segments |
|---|---:|---:|---:|---:|---:|---:|---:|
| 1 | 23.93 | 15.4 | 15.2 | 65.9 | 10.26 | 34.29 | 0 |
| 2 | 24.42 | 15.1 | 14.9 | 67.2 | 10.20 | 34.71 | 0 |
| 3 | 24.29 | 15.2 | 14.9 | 66.9 | 10.37 | 34.75 | 0 |
| **median** | 24.29 | 15.2 | 14.9 | 66.9 | 10.37 | 34.75 | 0 |
<!-- median_book_s=24.293366 -->
