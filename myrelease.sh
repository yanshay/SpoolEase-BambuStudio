#!/usr/bin/env bash
set -euo pipefail

WORKFLOW="publish_build_release.yml"
DEFAULT_REF="$(git branch --show-current 2>/dev/null || true)"

if ! command -v gh >/dev/null 2>&1; then
  echo "gh CLI is required." >&2
  exit 1
fi

gh auth status >/dev/null

read -r -p "Build run ID: " RUN_ID
if [[ ! "$RUN_ID" =~ ^[0-9]+$ ]]; then
  echo "Run ID must be numeric." >&2
  exit 1
fi

read -r -p "Workflow ref/branch [$DEFAULT_REF]: " REF
REF="${REF:-$DEFAULT_REF}"
if [ -z "$REF" ]; then
  echo "Workflow ref/branch is required." >&2
  exit 1
fi

CONCLUSION="$(gh run view "$RUN_ID" --json conclusion --jq .conclusion)"
STATUS="$(gh run view "$RUN_ID" --json status --jq .status)"
RUN_URL="$(gh run view "$RUN_ID" --json url --jq .url)"
RUN_BRANCH="$(gh run view "$RUN_ID" --json headBranch --jq .headBranch)"
RUN_SHA="$(gh run view "$RUN_ID" --json headSha --jq .headSha)"
RUN_WORKFLOW="$(gh run view "$RUN_ID" --json workflowName --jq .workflowName)"

if [ "$STATUS" != "completed" ] || [ "$CONCLUSION" != "success" ]; then
  echo "Run is not a successful completed run: status=$STATUS conclusion=$CONCLUSION" >&2
  exit 1
fi

echo
echo "Build run: $RUN_WORKFLOW"
echo "Branch:    $RUN_BRANCH"
echo "Commit:    $RUN_SHA"
echo "URL:       $RUN_URL"
echo

read -r -p "Release tag/title: " RELEASE_TAG
if [ -z "$RELEASE_TAG" ]; then
  echo "Release tag/title is required." >&2
  exit 1
fi

if ! git check-ref-format "refs/tags/$RELEASE_TAG"; then
  echo "Invalid Git tag: $RELEASE_TAG" >&2
  exit 1
fi

if gh release view "$RELEASE_TAG" >/dev/null 2>&1; then
  echo "Release already exists: $RELEASE_TAG" >&2
  exit 1
fi

if git ls-remote --exit-code --tags origin "refs/tags/$RELEASE_TAG" >/dev/null 2>&1; then
  echo "Git tag already exists on origin: $RELEASE_TAG" >&2
  exit 1
fi

echo
echo "Will trigger workflow: $WORKFLOW"
echo "Workflow ref:          $REF"
echo "Build run ID:          $RUN_ID"
echo "Release tag/title:     $RELEASE_TAG"
echo
read -r -p "Continue? [y/N] " CONFIRM
case "$CONFIRM" in
  y|Y|yes|YES) ;;
  *) echo "Cancelled."; exit 0 ;;
esac

gh workflow run "$WORKFLOW" --ref "$REF" -f "run_id=$RUN_ID" -f "release_tag=$RELEASE_TAG"

echo
echo "Triggered. Recent runs:"
gh run list --workflow "$WORKFLOW" --branch "$REF" --event workflow_dispatch --limit 5
