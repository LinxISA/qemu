#!/usr/bin/env bash
set -euo pipefail

LINUX_DIR="${LINUX_DIR:-$HOME/linux}"
UPSTREAM_URL="${UPSTREAM_URL:-https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git}"

if [[ -d "$LINUX_DIR/.git" ]]; then
  echo "[ok] linux git repo: $LINUX_DIR"
  echo "     branch: $(git -C \"$LINUX_DIR\" rev-parse --abbrev-ref HEAD)"
  exit 0
fi

if [[ -e "$LINUX_DIR" ]]; then
  echo "error: $LINUX_DIR exists but is not a git repo (missing .git)." >&2
  echo "       move it aside (e.g. mv ~/linux ~/linux.bak) and re-run." >&2
  exit 1
fi

LINUX_VERSION="${LINUX_VERSION:-}"
SOURCE_URL="${SOURCE_URL:-}"

if [[ -z "$LINUX_VERSION" || -z "$SOURCE_URL" ]]; then
  read -r LINUX_VERSION SOURCE_URL <<EOF
$(curl -fsSL https://www.kernel.org/releases.json | python3 -c 'import sys, json; data=json.load(sys.stdin); stable=next(r for r in data["releases"] if r.get("moniker")=="stable"); print(stable["version"], stable["source"])')
EOF
fi

echo "[info] latest stable: $LINUX_VERSION"
echo "[info] source: $SOURCE_URL"

TMPDIR="$(mktemp -d "${TMPDIR:-/tmp}/linux-setup.XXXXXX")"
trap 'rm -rf "$TMPDIR"' EXIT

ARCHIVE="$TMPDIR/linux-$LINUX_VERSION.tar.xz"

curl -fL --retry 5 --retry-delay 2 -o "$ARCHIVE" "$SOURCE_URL"
tar -xJf "$ARCHIVE" -C "$TMPDIR"
mv "$TMPDIR/linux-$LINUX_VERSION" "$LINUX_DIR"

echo "[git] init"
git -C "$LINUX_DIR" init

if ! git -C "$LINUX_DIR" config user.name >/dev/null; then
  git -C "$LINUX_DIR" config user.name "$(id -un 2>/dev/null || echo user)"
fi
if ! git -C "$LINUX_DIR" config user.email >/dev/null; then
  git -C "$LINUX_DIR" config user.email "$(id -un 2>/dev/null || echo user)@localhost"
fi

git -C "$LINUX_DIR" add -A
git -C "$LINUX_DIR" commit -m "Linux $LINUX_VERSION (kernel.org tarball)"

if ! git -C "$LINUX_DIR" rev-parse -q --verify "refs/tags/v$LINUX_VERSION" >/dev/null; then
  git -C "$LINUX_DIR" tag -a "v$LINUX_VERSION" -m "v$LINUX_VERSION"
fi

git -C "$LINUX_DIR" checkout -b linxisa/bringup

if git -C "$LINUX_DIR" remote get-url upstream >/dev/null 2>&1; then
  git -C "$LINUX_DIR" remote set-url upstream "$UPSTREAM_URL"
else
  git -C "$LINUX_DIR" remote add upstream "$UPSTREAM_URL"
fi

echo "[done] $LINUX_DIR @ $(git -C \"$LINUX_DIR\" rev-parse --abbrev-ref HEAD)"

