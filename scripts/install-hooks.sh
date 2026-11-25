#!/usr/bin/env bash
# Installs this repo's git hooks into .git/hooks.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

for hook in commit-msg pre-commit; do
    src="$repo_root/scripts/hooks/$hook"
    dest="$repo_root/.git/hooks/$hook"
    cp "$src" "$dest"
    chmod +x "$dest"
    echo "Installed $hook hook -> $dest"
done
