#!/bin/sh
set -eu

if [ "$#" -ne 3 ]; then
    echo "usage: $0 EXPORT_LIST BACKEND_MODULE OUTPUT_DEF" >&2
    exit 2
fi

list=$1
backend=$2
output=$3

case "$backend" in
    *[!A-Za-z0-9_.-]*|'') echo "invalid backend module name" >&2; exit 2 ;;
esac

tmp="${output}.tmp"
trap 'rm -f "$tmp"' EXIT HUP INT TERM
{
    echo 'LIBRARY d3d12'
    echo 'EXPORTS'
    echo '    D3D12CreateDevice @101'
    echo '    D3D12GetInterface'
    while read -r symbol ordinal extra; do
        case "$symbol" in ''|'#'*) continue ;; esac
        case "$symbol" in *[!A-Za-z0-9_@?]*) echo "invalid export: $symbol" >&2; exit 2 ;; esac
        case "$ordinal" in @*) ;; *) echo "invalid ordinal: $ordinal" >&2; exit 2 ;; esac
        case "${ordinal#@}" in ''|*[!0-9]*) echo "invalid ordinal: $ordinal" >&2; exit 2 ;; esac
        if [ -n "${extra:-}" ]; then echo "unexpected export data: $extra" >&2; exit 2; fi
        printf '    %s=%s.%s %s\n' "$symbol" "$backend" "$symbol" "$ordinal"
    done < "$list"
} > "$tmp"
mv "$tmp" "$output"
trap - EXIT HUP INT TERM
