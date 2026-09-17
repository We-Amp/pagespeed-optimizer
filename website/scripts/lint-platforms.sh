#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Validate platform blocks in 1.1 docs markdown files.
# Checks that <div data-platform> tags have blank lines before and after
# for correct markdown parsing inside them.
set -euo pipefail

DIR="$(cd "$(dirname "$0")/../src/content/docs-1.1" && pwd)"
EXIT=0

for f in "$DIR"/*.md; do
  [ -f "$f" ] || continue
  name="$(basename "$f")"
  lineno=0
  prev_line=""

  while IFS= read -r line; do
    lineno=$((lineno + 1))

    # Check opening <div data-platform
    if echo "$line" | grep -q '<div data-platform'; then
      if [ -n "$prev_line" ]; then
        echo "ERROR: $name:$lineno — <div data-platform> must have a blank line before it"
        EXIT=1
      fi
    fi

    # Check closing </div> after a platform block
    # We track if previous line had content before </div>
    if echo "$line" | grep -q '</div>' && echo "$prev_line" | grep -qv '^$'; then
      # Check if we're inside a platform block (simple heuristic: look backwards)
      if grep -q 'data-platform' "$f"; then
        echo "WARNING: $name:$lineno — </div> should have a blank line before it for markdown parsing"
      fi
    fi

    prev_line="$line"
  done < "$f"

  # Check that platform blocks have matching comments
  opens=$(grep -c '<div data-platform' "$f" 2>/dev/null || true)
  closes=$(grep -c '</div>' "$f" 2>/dev/null || true)
  if [ "$opens" -gt 0 ] && [ "$opens" -ne "$closes" ]; then
    echo "ERROR: $name — mismatched platform blocks: $opens opens, $closes closes"
    EXIT=1
  fi
done

if [ $EXIT -eq 0 ]; then
  echo "All platform blocks validated."
fi
exit $EXIT
