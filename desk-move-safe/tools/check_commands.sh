#!/usr/bin/env bash
# Fail if the console and docs/commands.md disagree.
#
# Commands: help() in src/console.c prints one line per command,
# "  <signature>  <text>" with two or more spaces between. docs/commands.md has a
# row per command in a table whose header starts "| Command |"; its first cell
# is the same signature in backticks, pipes escaped as \|. Every top-level word
# the dispatcher accepts must also appear in help().
#
# Parameters: every name in the P[] table of src/params.c has a row in a table
# whose header starts "| Parameter |", and every row there is a name in P[].
#
# Other tables in the doc (explanations, examples) are not read.
set -euo pipefail
cd "$(dirname "$0")/.."

src=src/console.c
par=src/params.c
doc=docs/commands.md
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

rows() {   # $1 header word: first-cell backticked values of those tables' rows
  awk -v h="$1" '
    /^\| / && !intable { intable = 1; mine = ($0 ~ "^\\| " h " \\|"); next }
    !/^\|/            { intable = 0; mine = 0; next }
    mine' "$doc" \
  | grep -E '^\| `[^`]+` \|' | sed -E 's/^\| `([^`]+)`.*/\1/; s/\\\|/|/g' | sort -u
}

# Commands printed by help(), and documented above the parameters section.
awk '/^static void help\(void\)/,/^}/' "$src" \
  | grep -E '^[[:space:]]*"  [^ -]' \
  | sed -E 's/^[[:space:]]*"  //; s/ {2,}.*//; s/\\n"?,?$//' \
  | sort -u > "$tmp/help"
rows Command > "$tmp/doc"

# Top-level words the dispatcher accepts, and the first words help() offers.
grep -oE 'strcmp\(cmd, "[^"]+"\)' "$src" | sed -E 's/.*"([^"]+)".*/\1/' | sort -u > "$tmp/cmds"
awk '{print $1}' "$tmp/help" | sort -u > "$tmp/offered"

# Parameters in params.c's table, and documented.
awk '/^static const param_t P\[\]/,/^};/' "$par" \
  | grep -oE '^[[:space:]]*\{ "[a-z0-9_]+"' | sed -E 's/.*"([^"]+)"/\1/' | sort -u > "$tmp/params"
rows Parameter > "$tmp/docparams"

fail=0
report() {   # $1 what, $2 left file, $3 left name, $4 right file, $5 right name
  if ! diff -q "$2" "$4" >/dev/null; then
    echo "check_commands: $1 disagree:"
    comm -23 "$2" "$4" | sed "s|^|  in $3, not in $5: |"
    comm -13 "$2" "$4" | sed "s|^|  in $5, not in $3: |"
    fail=1
  fi
}
report "commands"   "$tmp/help"   "help()"   "$tmp/doc"        "$doc"
report "parameters" "$tmp/params" "$par"     "$tmp/docparams"  "$doc"

missing=$(comm -23 "$tmp/cmds" "$tmp/offered" || true)
if [ -n "$missing" ]; then
  echo "check_commands: accepted by dispatch() but not in help():"
  echo "$missing" | sed 's/^/  /'
  fail=1
fi
[ $fail -eq 0 ] && echo "check_commands: $(wc -l < "$tmp/help" | tr -d ' ') commands and $(wc -l < "$tmp/params" | tr -d ' ') parameters agree with $doc"
exit $fail
