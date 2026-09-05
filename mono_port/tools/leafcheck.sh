#!/usr/bin/env bash
# =====================================================================
#  leafcheck.sh - which utils/ objects can be linked without metadata?
# ---------------------------------------------------------------------
#  Mono's utils/ is not a self-contained bottom layer.  utils/mono-error.c
#  builds MonoException objects, utils/mono-threads-posix.c calls
#  mono_gc_pthread_create -- both live in metadata/.  Upstream never
#  notices because everything lands in one libmono.so.
#
#  For a Phase 0.4 smoke test we need the subset that IS self-contained,
#  so this walks the dependency closure of each utils object over
#  {utils, eglib, PAL} and prints those whose closure stays inside.
#
#  usage: bash tools/leafcheck.sh [symbol ...]
#         with no args  -> classify every utils object
#         with symbols  -> print the closure needed by those symbols
# =====================================================================
set -u
cd "$(dirname "$0")/.."
BUILD=build

[ -d "$BUILD" ] || { echo "no $BUILD, run make runtime first" >&2; exit 1; }

# --- symbol tables ---------------------------------------------------
# provider[sym] = object file that defines it, for the linkable set
declare -A provider
declare -A undef_of

# Synthesised by the linker itself, never by an object: counting these as
# unresolved would mark almost every PIC object as non-leaf.
is_linker_symbol () {
    case "$1" in
        _GLOBAL_OFFSET_TABLE_|__x86.get_pc_thunk.*|_DYNAMIC|__bss_start|_edata|_end) return 0 ;;
        *) return 1 ;;
    esac
}

# build/utils_smoke.o is a test driver, not a member of utils/; it only
# matches the glob by accident of naming.
for o in "$BUILD"/utils_*.o "$BUILD"/eglib_*.o "$BUILD"/pal_*.o; do
    [ -e "$o" ] || continue
    [ "$o" = "$BUILD/utils_smoke.o" ] && continue
    while read -r _ t s; do
        case "$t" in
            T|D|B|R|W|V|G|S) [ -n "${provider[$s]+x}" ] || provider[$s]="$o" ;;
        esac
    done < <(nm -g --defined-only "$o" 2>/dev/null)
    undef_of["$o"]="$(nm -g --undefined-only "$o" 2>/dev/null | awk '{print $2}' | tr '\n' ' ')"
done

# --- closure walk ----------------------------------------------------
# closure_of <object...>  -> sets CLOSURE_OK / CLOSURE_MISS
closure_of () {
    local -A seen=() missing=()
    local queue=("$@") cur sym dep
    while [ ${#queue[@]} -gt 0 ]; do
        cur="${queue[0]}"; queue=("${queue[@]:1}")
        [ -n "${seen[$cur]+x}" ] && continue
        seen["$cur"]=1
        for sym in ${undef_of[$cur]:-}; do
            is_linker_symbol "$sym" && continue
            dep="${provider[$sym]:-}"
            if [ -z "$dep" ]; then
                missing["$sym"]=1
            elif [ -z "${seen[$dep]+x}" ]; then
                queue+=("$dep")
            fi
        done
    done
    CLOSURE_MISS="${!missing[*]}"
    CLOSURE_N=${#seen[@]}
}

# --- mode 2: closure of the given symbols ----------------------------
if [ $# -gt 0 ]; then
    roots=()
    for sym in "$@"; do
        dep="${provider[$sym]:-}"
        if [ -z "$dep" ]; then
            echo "?? $sym  -- defined nowhere in utils/eglib/pal"
        else
            roots+=("$dep")
        fi
    done
    [ ${#roots[@]} -eq 0 ] && exit 1
    closure_of "${roots[@]}"
    echo "closure: $CLOSURE_N objects"
    if [ -n "$CLOSURE_MISS" ]; then
        echo "UNRESOLVED (needs metadata/ or mini/):"
        for s in $CLOSURE_MISS; do echo "    $s"; done
        exit 1
    fi
    echo "self-contained: links against utils+eglib+pal alone"
    exit 0
fi

# --- mode 1: classify every utils object -----------------------------
leaf=0; dep=0
: > "$BUILD/utils_leaves.txt"
for o in "$BUILD"/utils_*.o; do
    [ "$o" = "$BUILD/utils_smoke.o" ] && continue
    closure_of "$o"
    name=$(basename "$o" .o); name=${name#utils_}
    if [ -z "$CLOSURE_MISS" ]; then
        printf '  LEAF  %-34s (closure %d)\n' "$name" "$CLOSURE_N"
        echo "$name" >> "$BUILD/utils_leaves.txt"
        leaf=$((leaf+1))
    else
        n=$(echo "$CLOSURE_MISS" | wc -w)
        printf '  dep   %-34s needs %d external symbol(s), e.g. %s\n' \
               "$name" "$n" "$(echo "$CLOSURE_MISS" | awk '{print $1}')"
        dep=$((dep+1))
    fi
done
echo "-------------------------------------------------"
echo "self-contained : $leaf     needs metadata/mini : $dep"
echo "leaf list -> $BUILD/utils_leaves.txt"
