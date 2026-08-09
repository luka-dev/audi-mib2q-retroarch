This source directory intentionally contains no independent override config.

During ./build.sh, pkg/retroarch.cfg is copied to:
  build/sd_card/retroarch/config/retroarch.cfg

At runtime, /mnt/app/root/retroarch/ra.sh creates the same writable copy when a
new or blank SD card is inserted. RetroArch uses that SD copy as its primary
config, so Save Configuration never targets the read-only factory template.
