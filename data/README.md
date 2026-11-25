# data/

Raw and derived market data (ITCH 5.0 feeds, decoded snapshots, etc.) lives here
but is **not** checked into git — see `.gitignore`. Only this README is tracked.

Expected layout once data is placed here:

```
data/
  raw/        # original .itch / .itch.gz TotalView-ITCH 5.0 files
  decoded/    # intermediate decoded message dumps, if any
```

Nasdaq TotalView-ITCH 5.0 sample files can be obtained from Nasdaq's historical
data distribution. Document the exact source and date of any file you use in
`PROGRESS.md` so results stay reproducible.
