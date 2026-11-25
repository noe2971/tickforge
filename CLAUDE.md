# Tickforge — rules for Claude

1. Correctness first: every component gets tests, and performance claims need
   a benchmark that's committed.
2. Never use future data in the simulator: a simulated order may only react
   to market data timestamped before it acts.
3. Report only measured numbers; never estimate a benchmark or a result.
4. Run the tests before every commit and never commit if they fail.
5. Commit after each completed step with a conventional commit message, then
   push to origin main.
6. NEVER add Co-Authored-By lines, "Generated with Claude Code" lines or any
   AI attribution to commits, PRs or files.
7. Before locking any experiment, write its rules in `PROGRESS.md` and commit
   them first, so results can't influence them.
8. After every change, explain it to me in plain English, because I'm a
   first-year student who must explain this code in quant developer
   interviews.
