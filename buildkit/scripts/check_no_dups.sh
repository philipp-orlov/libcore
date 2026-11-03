#!/usr/bin/env bash
# Fails when a helper that belongs to libbase is defined again elsewhere. Usage: check_no_dups.sh <root> [<root>...]
# Each root is searched recursively; libcore's own libbase directory is exempt.
set -u
patterns=(
  '2166136261u'                                   # FNV-1a 32 offset basis
  '0x9E3779B97F4A7C15'                            # 64-bit golden ratio constant (hashBytes64 and friends)
  'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789\+/'   # base64 alphabet
  'bool +isValidUtf8 *\('                         # UTF-8 validators
  '0x67452301'                                    # SHA-1 initial state
)
status=0
for root in "$@"; do
  for p in "${patterns[@]}"; do
    hits=$(grep -rEn --include='*.cpp' --include='*.hpp' --include='*.h' --include='*.cc' -e "$p" "$root" 2>/dev/null \
           | grep -v '/libbase/' | grep -v '/build' | grep -v '/third_party/' || true)
    if [ -n "$hits" ]; then
      echo "duplicate of a libbase helper (pattern: $p):"
      echo "$hits" | sed 's/^/  /'
      status=1
    fi
  done
done
exit $status
