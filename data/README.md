# data/

Raw and derived market data (ITCH 5.0 feeds, decoded snapshots, etc.) lives here
but is **not** checked into git — see `.gitignore`. Only this README is tracked.

Expected layout once data is placed here:

```
data/
  raw/        # original .itch / .itch.gz TotalView-ITCH 5.0 files
  decoded/    # intermediate decoded message dumps, if any
```

Get the sample day with `tools/download_itch.sh` (resumable, verified). It saves
`data/01302019.NASDAQ_ITCH50.gz` (4,764,426,091 bytes) straight from
`https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/`. `tools/itch_filter` writes compact
per-symbol `.tfc` files here as well (format: `include/tickforge/itch/compact_file.hpp`).

Nasdaq TotalView-ITCH 5.0 sample files can be obtained from Nasdaq's historical
data distribution. Document the exact source and date of any file you use in
`PROGRESS.md` so results stay reproducible.
