#!/usr/bin/env bash
set -euo pipefail

WORKFLOW="publish_build_release.yml"

get_default_ref() {
  local ref

  ref="$(git branch --show-current 2>/dev/null || true)"
  if [ -n "$ref" ]; then
    printf '%s\n' "$ref"
    return
  fi

  if command -v jj >/dev/null 2>&1; then
    ref="$(jj log -r 'latest(::@ & bookmarks(), 1)' --no-graph -T 'bookmarks.join(" ")' 2>/dev/null || true)"
    ref="${ref%% *}"
    if [ -n "$ref" ]; then
      printf '%s\n' "$ref"
      return
    fi
  fi
}

normalize_version() {
  local raw="$1"
  local part
  local normalized=""
  local -a parts

  IFS='.' read -r -a parts <<< "$raw"
  for part in "${parts[@]}"; do
    if [[ ! "$part" =~ ^[0-9]+$ ]]; then
      echo "Invalid version component: $part" >&2
      exit 1
    fi
    normalized+="${normalized:+.}$((10#$part))"
  done

  printf '%s\n' "$normalized"
}

get_bambu_version() {
  local line

  while IFS= read -r line; do
    if [[ "$line" =~ set\(SLIC3R_VERSION[[:space:]]+\"([0-9.]+)\"\) ]]; then
      normalize_version "${BASH_REMATCH[1]}"
      return
    fi
  done < version.inc

  echo "Could not find SLIC3R_VERSION in version.inc." >&2
  exit 1
}

suggest_release_tag() {
  local version="$1"
  local prefix="${version}-SpoolEase"
  local max=0
  local tag suffix n

  while IFS= read -r tag; do
    if [[ "$tag" == "$prefix".* ]]; then
      suffix="${tag#${prefix}.}"
      if [[ "$suffix" =~ ^[0-9]+$ ]]; then
        n=$((10#$suffix))
        if (( n > max )); then
          max=$n
        fi
      fi
    fi
  done < <(gh release list --limit 1000 --json tagName --jq '.[].tagName')

  printf '%s.%d\n' "$prefix" "$((max + 1))"
}

format_github_time() {
  local timestamp="$1"
  local epoch formatted

  if epoch="$(TZ=UTC date -j -f '%Y-%m-%dT%H:%M:%SZ' "$timestamp" '+%s' 2>/dev/null)" && \
    formatted="$(date -r "$epoch" '+%Y-%m-%d %H:%M:%S %Z' 2>/dev/null)"; then
    printf '%s\n' "$formatted"
    return
  fi

  if formatted="$(date -d "$timestamp" '+%Y-%m-%d %H:%M:%S %Z' 2>/dev/null)"; then
    printf '%s\n' "$formatted"
    return
  fi

  timestamp="${timestamp/T/ }"
  printf '%s\n' "${timestamp%Z} UTC"
}

DEFAULT_REF="$(get_default_ref)"

if ! command -v gh >/dev/null 2>&1; then
  echo "gh CLI is required." >&2
  exit 1
fi

gh auth status >/dev/null

SUGGESTED_RUN_ID="$(gh run list --workflow build_all.yml --status success --limit 1 --json databaseId --jq '.[0].databaseId // ""')"
if [ -n "$SUGGESTED_RUN_ID" ]; then
  SUGGESTED_RUN_CREATED="$(gh run view "$SUGGESTED_RUN_ID" --json createdAt --jq .createdAt)"
  SUGGESTED_RUN_BRANCH="$(gh run view "$SUGGESTED_RUN_ID" --json headBranch --jq .headBranch)"
  SUGGESTED_RUN_URL="$(gh run view "$SUGGESTED_RUN_ID" --json url --jq .url)"

  echo "Latest successful Build all run:"
  echo "Run ID:  $SUGGESTED_RUN_ID"
  echo "Branch:  $SUGGESTED_RUN_BRANCH"
  echo "Created: $(format_github_time "$SUGGESTED_RUN_CREATED")"
  echo "URL:     $SUGGESTED_RUN_URL"
  echo
fi

read -r -p "Build run ID [$SUGGESTED_RUN_ID]: " RUN_ID
RUN_ID="${RUN_ID:-$SUGGESTED_RUN_ID}"
if [[ ! "$RUN_ID" =~ ^[0-9]+$ ]]; then
  echo "Run ID must be numeric." >&2
  exit 1
fi

REF="$DEFAULT_REF"
if [ -z "$REF" ]; then
  echo "Could not detect workflow ref/branch." >&2
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

BAMBU_VERSION="$(get_bambu_version)"
SUGGESTED_RELEASE_TAG="$(suggest_release_tag "$BAMBU_VERSION")"

echo "BambuStudio version: $BAMBU_VERSION"
read -r -p "Release tag/title [$SUGGESTED_RELEASE_TAG]: " RELEASE_TAG
RELEASE_TAG="${RELEASE_TAG:-$SUGGESTED_RELEASE_TAG}"
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
