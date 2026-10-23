#!/usr/bin/env bash
set -euo pipefail

WORKFLOW="build_all.yml"

if [ "$#" -gt 1 ]; then
  echo "Usage: $0 [branch]" >&2
  exit 1
fi

REF="${1:-$(git branch --show-current)}"
if [ -z "$REF" ]; then
  read -r -p "Branch to build [spoolease]: " REF
  REF="${REF:-spoolease}"
fi

if ! command -v gh >/dev/null 2>&1; then
  echo "gh CLI is required." >&2
  exit 1
fi

gh auth status >/dev/null

if ! git ls-remote --exit-code --heads origin "refs/heads/$REF" >/dev/null 2>&1; then
  echo "Remote branch does not exist: $REF" >&2
  exit 1
fi

gh workflow run "$WORKFLOW" --ref "$REF" -f "build-deps-only=false"

echo "Triggered Build all for branch: $REF"
gh run list --workflow "$WORKFLOW" --branch "$REF" --event workflow_dispatch --limit 5
