MHI2Q RetroArch writable SD-card image
Runtime root: /fs/sda0/retroarch

Copy the CONTENTS of build/sd_card to the root of a FAT32 SD card. The image
contains the current PS1/GBA/N64 games, PS1 BIOS, databases, cheats, box art and a
known-good initial config. The application binary, cores, Audi/Ozone assets,
controller mappings, rumble profiles and HMI JAR live in build/mnt_app.

Portable user/runtime tree:
  config/*.cfg          writable frontend/core config seeded from app factory
  config/remaps/        per-core/per-game controller remaps
  autoconfig/*.cfg      per-card controller overrides (highest priority)
  autoconfig/qnx/       factory profiles seeded by ra.sh
  system/               BIOS and other core system files
  ps1/ gba/ n64/ roms/  recursively scanned game content
  saves/ states/        SRAM/memory cards and savestates
  playlists/            generated automatically from content-rules.cfg
  thumbnails/           offline box art
  info/                 writable core-info metadata/cache
  database/rdb/         compiled game databases
  cheats/               offline cheat collection
  logs/ screenshots/    bounded diagnostics and captures
  downloads/ filters/ wallpapers/ overlays/  optional user additions

Factory-reset behaviour:
  - Existing card: config, saves, states and playlists remain on that card.
  - New/blank card: ra.sh creates the tree and copies the immutable factory
    config, core-info and controller-profile seed from /mnt/app.
  - No card: RetroArch uses a volatile config under /tmp; /mnt/app stays ro.

Swapping cards takes effect on the next RetroArch launch. Do not remove a card
while a game is actively writing SRAM or a savestate.
