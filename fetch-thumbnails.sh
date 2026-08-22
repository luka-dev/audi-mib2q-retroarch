#!/usr/bin/env bash
# Download missing RetroArch box-art PNGs for a PS1, GBA or N64 games directory.
#
# Usage:
#   ./fetch-thumbnails.sh GAMES_DIR [ps1|gba|n64|psp|auto] [THUMBNAILS_DIR]
#
# With only GAMES_DIR, the system is detected from file extensions and images
# are written directly into the ready-to-deploy SD-card tree.
set -euo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
GAMES_DIR=${1:-}
SYSTEM=${2:-auto}
THUMBNAILS_DIR=${3:-"$SCRIPT_DIR/build/sd_card/retroarch/thumbnails"}

usage() {
   echo "Usage: $0 GAMES_DIR [ps1|gba|n64|psp|auto] [THUMBNAILS_DIR]" >&2
   exit 2
}

[ -n "$GAMES_DIR" ] || usage
[ -d "$GAMES_DIR" ] || {
   echo "Games directory does not exist: $GAMES_DIR" >&2
   exit 1
}
command -v curl >/dev/null 2>&1 || {
   echo "curl is required" >&2
   exit 1
}

if [ "$SYSTEM" = auto ]; then
   if find "$GAMES_DIR" -type f -iname '*.gba' -print -quit | grep -q .; then
      SYSTEM=gba
   elif find "$GAMES_DIR" -type f \( -iname '*.z64' -o -iname '*.n64' \
         -o -iname '*.v64' \) -print -quit | grep -q .; then
      SYSTEM=n64
   elif find "$GAMES_DIR" -type f \( -iname '*.iso' -o -iname '*.cso' \) \
         -print -quit | grep -q .; then
      SYSTEM=psp
   elif find "$GAMES_DIR" -type f \( -iname '*.cue' -o -iname '*.chd' \
         -o -iname '*.pbp' -o -iname '*.m3u' \) -print -quit | grep -q .; then
      SYSTEM=ps1
   else
      echo "Cannot detect system in $GAMES_DIR (expected PS1, GBA, N64 or PSP content)" >&2
      exit 1
   fi
fi

case "$SYSTEM" in
   ps1)
      PLAYLIST='Sony - PlayStation'
      REPOSITORY='Sony_-_PlayStation'
      ;;
   gba)
      PLAYLIST='Nintendo - Game Boy Advance'
      REPOSITORY='Nintendo_-_Game_Boy_Advance'
      ;;
   n64)
      PLAYLIST='Nintendo - Nintendo 64'
      REPOSITORY='Nintendo_-_Nintendo_64'
      ;;
   psp)
      PLAYLIST='Sony - PlayStation Portable'
      REPOSITORY='Sony_-_PlayStation_Portable'
      ;;
   *)
      echo "Unsupported system: $SYSTEM (use ps1, gba, n64, psp or auto)" >&2
      exit 1
      ;;
esac

BOXART_DIR="$THUMBNAILS_DIR/$PLAYLIST/Named_Boxarts"
mkdir -p "$BOXART_DIR"
TEMP_FILE=$(mktemp "${TMPDIR:-/tmp}/retroarch-boxart.XXXXXX")
INDEX_FILE=''
cleanup() {
   rm -f -- "$TEMP_FILE"
   [ -z "$INDEX_FILE" ] || rm -f -- "$INDEX_FILE"
}
trap cleanup EXIT

# RetroArch thumbnail servers require URL-encoded UTF-8 paths. Encoding every
# byte keeps this independent of Python, jq and the host curl version.
urlencode() {
   printf '%s' "$1" | LC_ALL=C od -An -tx1 | tr -d ' \n' | sed 's/../%&/g'
}

thumbnail_safe_name() {
   local value=$1 bad
   for bad in '&' '*' '/' ':' '`' '<' '>' '?' '\\' '|'; do
      value=${value//"$bad"/_}
   done
   printf '%s' "$value"
}

# The in-process playlist generator deliberately hides database suffixes such
# as (USA), (Europe), revisions and dump tags. Keep thumbnail names identical
# to those short labels.
short_label() {
   local value=$1
   while [[ "$value" =~ ^(.*)[[:space:]]+\([^()]*\)$ ]]; do
      value=${BASH_REMATCH[1]}
   done
   while [[ "$value" =~ ^(.*)[[:space:]]+\[[^][]*\]$ ]]; do
      value=${BASH_REMATCH[1]}
   done
   printf '%s' "$value"
}

download_candidate() {
   local remote_name=$1 target=$2 encoded url signature
   remote_name=$(thumbnail_safe_name "$remote_name")
   encoded=$(urlencode "$remote_name.png")
   url="https://raw.githubusercontent.com/libretro-thumbnails/$REPOSITORY/master/Named_Boxarts/$encoded"
   if ! curl -fLs --retry 2 --connect-timeout 10 "$url" -o "$TEMP_FILE"; then
      return 1
   fi
   signature=$(LC_ALL=C od -An -tx1 -N8 "$TEMP_FILE" | tr -d ' \n')
   [ "$signature" = 89504e470d0a1a0a ] || return 1
   mv -f -- "$TEMP_FILE" "$target"
   TEMP_FILE=$(mktemp "${TMPDIR:-/tmp}/retroarch-boxart.XXXXXX")
   return 0
}

load_repository_index() {
   [ -n "$INDEX_FILE" ] && [ -s "$INDEX_FILE" ] && return 0
   INDEX_FILE=$(mktemp "${TMPDIR:-/tmp}/retroarch-boxart-index.XXXXXX")
   printf 'index      %s\n' "$PLAYLIST" >&2
   curl -fLs --retry 2 --connect-timeout 10 \
      "https://api.github.com/repos/libretro-thumbnails/$REPOSITORY/git/trees/master?recursive=1" \
      -o "$INDEX_FILE"
}

# Pretty local filenames omit region/revision suffixes. If direct candidates
# fail, resolve the short name against the official repository index. Prefer a
# USA image, then World and Europe, while still accepting the first exact short
# title match for games that only have another region.
find_index_candidate() {
   local wanted=$1 remote remote_base remote_short score remote_length
   local best_score=0 best_length=999999 best=''
   INDEXED_CANDIDATE=''
   load_repository_index || return 1
   while IFS= read -r remote; do
      remote_base=${remote%.png}
      remote_short=$(short_label "$remote_base")
      [ "$remote_short" = "$wanted" ] || continue
      score=1
      case "$remote_base" in
         *'(USA)'*|*'(USA,'*) score=4 ;;
         *'(World)'*) score=3 ;;
         *'(Europe)'*|*'(Europe,'*) score=2 ;;
      esac
      remote_length=${#remote_base}
      if [ "$score" -gt "$best_score" ] \
            || { [ "$score" -eq "$best_score" ] \
                 && [ "$remote_length" -lt "$best_length" ]; }; then
         best_score=$score
         best_length=$remote_length
         best=$remote_base
      fi
   done < <(sed -n 's/^[[:space:]]*"path": "Named_Boxarts\/\(.*\.png\)",$/\1/p' "$INDEX_FILE")
   [ -n "$best" ] || return 1
   INDEXED_CANDIDATE=$best
}

candidate_add() {
   local candidate=$1 existing
   [ -n "$candidate" ] || return 0
   for existing in "${CANDIDATES[@]:-}"; do
      [ "$candidate" = "$existing" ] && return 0
   done
   CANDIDATES+=("$candidate")
}

downloaded=0
present=0
missing=0
seen=0

while IFS= read -r -d '' content; do
   seen=$((seen + 1))
   filename=$(basename -- "$content")
   base=${filename%.*}
   label=$(short_label "$base")
   safe_label=$(thumbnail_safe_name "$label")
   target="$BOXART_DIR/$safe_label.png"

   if [ -s "$target" ]; then
      printf 'present    %s\n' "$label"
      present=$((present + 1))
      continue
   fi

   CANDIDATES=()
   candidate_add "$base"

   # Renamed .cue files still retain the original regional BIN filename. This
   # is normally the strongest match for the official Libretro image name.
   if [[ "$content" == *.[cC][uU][eE] ]]; then
      while IFS= read -r referenced; do
         referenced=${referenced##*/}
         candidate_add "${referenced%.*}"
      done < <(sed -n 's/^[[:space:]]*FILE[[:space:]]*"\(.*\)".*/\1/p' "$content")
   fi

   candidate_add "$(basename -- "$(dirname -- "$content")")"
   candidate_add "$label"

   found=false
   for candidate in "${CANDIDATES[@]}"; do
      if download_candidate "$candidate" "$target"; then
         printf 'downloaded %s  <-  %s\n' "$label" "$candidate"
         downloaded=$((downloaded + 1))
         found=true
         break
      fi
   done

   if [ "$found" = false ]; then
      if find_index_candidate "$label" \
            && download_candidate "$INDEXED_CANDIDATE" "$target"; then
         printf 'downloaded %s  <-  %s\n' "$label" "$INDEXED_CANDIDATE"
         downloaded=$((downloaded + 1))
         found=true
      fi
   fi

   if [ "$found" = false ]; then
      printf 'missing    %s\n' "$label" >&2
      missing=$((missing + 1))
   fi
done < <(
   case "$SYSTEM" in
      gba)
         find "$GAMES_DIR" -type f -iname '*.gba' -print0
         ;;
      n64)
         find "$GAMES_DIR" -type f \( -iname '*.z64' -o -iname '*.n64' \
            -o -iname '*.v64' \) -print0
         ;;
      ps1)
         find "$GAMES_DIR" -type f \( -iname '*.cue' -o -iname '*.chd' \
            -o -iname '*.pbp' -o -iname '*.m3u' \) -print0
         ;;
      psp)
         find "$GAMES_DIR" -type f \( -iname '*.iso' -o -iname '*.cso' \
            -o -iname '*.pbp' -o -iname '*.chd' \) -print0
         ;;
   esac
)

[ "$seen" -gt 0 ] || {
   echo "No supported game files found in $GAMES_DIR" >&2
   exit 1
}

printf '\nBox art: %d downloaded, %d already present, %d not found\n' \
   "$downloaded" "$present" "$missing"
printf 'Output: %s\n' "$BOXART_DIR"
[ "$missing" -eq 0 ]
