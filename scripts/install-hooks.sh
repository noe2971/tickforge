#!/usr/bin/env bash
# Installs this repo's git hooks into .git/hooks.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
src="$repo_root/scripts/hooks/commit-msg"
dest="$repo_root/.git/hooks/commit-msg"

cp "$src" "$dest"
chmod +x "$dest"

echo "Installed commit-msg hook -> $dest"
