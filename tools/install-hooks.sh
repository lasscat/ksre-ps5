#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Install local git hooks that run the publication check before every commit
# (staged content) and push (outgoing commits). Hooks live in .git/hooks and
# are never published. Re-run after cloning.
set -euo pipefail
# shellcheck source=lib/common.sh
. "$(dirname "$0")/lib/common.sh"

hooks="$(git -C "$KSRE_PS5_ROOT" rev-parse --git-path hooks)"
case "$hooks" in /*) ;; *) hooks="${KSRE_PS5_ROOT}/${hooks}" ;; esac
mkdir -p "$hooks"

cat > "${hooks}/pre-commit" <<'EOF'
#!/bin/sh
exec python3 "$(git rev-parse --show-toplevel)/tools/publication_check.py" --staged
EOF
cat > "${hooks}/pre-push" <<'EOF'
#!/bin/sh
# Check every commit about to be pushed (all history if no upstream yet).
exec python3 "$(git rev-parse --show-toplevel)/tools/publication_check.py" --outgoing
EOF
chmod +x "${hooks}/pre-commit" "${hooks}/pre-push"
echo "installed pre-commit and pre-push publication checks in ${hooks}"
