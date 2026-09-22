---
title: Content discovery - automatic playlists from content-rules.cfg
tags: [native, playlists]
status: verified-hardware
sources:
  - src/tasks/task_content_discovery.c
  - pkg/content-rules.cfg
reconciles:
  - README.md "Automatic game playlists"
---

# Content discovery - automatic playlists from content-rules.cfg

A background task (`task_content_discovery.c`) runs when `RA_CONTENT_RULES` points at a rules
file. It scans only the declared roots, merges matches into one `.lpl` per rule, refreshes Ozone
without blocking, needs no database hashes and never asks the user to pick a core. The load-content
menus are hidden ([[configuration]]).

## `content-rules.cfg` (shipped)

```ini
root0 = "/fs/sda0/retroarch"
root1 = "/fs/sdb0/retroarch"

rule0_directory = "ps1"   rule0_playlist = "Sony - PlayStation"          rule0_extensions = "cue|chd|pbp"
rule0_core = "pcsx_rearmed_libretro"          rule0_core_name = "PCSX-ReARMed"
rule1_directory = "gba"   rule1_playlist = "Nintendo - Game Boy Advance" rule1_extensions = "gba|bin"
rule1_core = "gpsp_libretro"                  rule1_core_name = "gpSP"
rule2_directory = "n64"   rule2_playlist = "Nintendo - Nintendo 64"      rule2_extensions = "z64|n64|v64"
rule2_core = "mupen64plus_next_gles2_libretro" rule2_core_name = "Mupen64Plus-Next"
; every rule: recursive = "true", label_mode = "keep_disc"
```

Limits: 4 roots, 16 rules. `label_mode`: `filename`, `clean`, `keep_disc` (strip region/revision
tags, keep multi-disc suffixes). PS1 track `.bin` files are ignored (only `.cue/.chd/.pbp`).

## Behaviour

- Playlists are written to the primary card's `playlists/` and are **not** rewritten when content is
  unchanged.
- After a successful scan of a directory, stale entries under it are removed from History and
  Favorites. An absent card or unreadable directory is *not* proof of deletion, so pulling the
  second card does not erase Favorites.
- Notifications report scan start, updates, unchanged library, errors.
- Adding a system = a new `ruleN_*` block + the core + its info file; no scanner code changes.

Optional 64DD BIOS: `system/Mupen64plus/IPL.n64`. PS1 BIOS in `system/`.
