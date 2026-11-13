#!/system/bin/sh
# The tablet app started afresh, the game's memory range held first (android/guestreserve.c)
HERE="$(cd "$(dirname "$0")" && pwd)"
export LD_PRELOAD="$HERE/libguestreserve.so"
exec "$@"
