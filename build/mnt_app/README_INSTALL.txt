MHI2Q RetroArch immutable /mnt/app image

Copy the CONTENTS of this mnt_app directory to /mnt/app while the app image is
explicitly mounted writable for deployment. Runtime operation must leave
/mnt/app read-only.

Installed paths:
  /mnt/app/root/retroarch/retroarch              QNX ARM frontend
  /mnt/app/root/retroarch/cores/*.so             gpSP and PCSX-ReARMed
  /mnt/app/root/retroarch/lib/*.so*              private runtime libraries
  /mnt/app/root/retroarch/assets/                Ozone + Audi UI/font/wallpaper
  /mnt/app/root/retroarch/autoconfig/qnx/*.cfg   controller mappings
  /mnt/app/root/retroarch/rumble/qnx/*.cfg       HID output reports
  /mnt/app/root/retroarch/info/*.info            seed core metadata
  /mnt/app/root/retroarch/retroarch.cfg          immutable factory template
  /mnt/app/root/retroarch/ra.sh                  SD-aware launcher
  /mnt/app/eso/hmi/lsd/jars/ra_mhi2q.jar         Games HMI state/audio hook

ra.sh never asks RetroArch to save this factory config. On a new SD it copies
the template to /fs/sda0/retroarch/config/retroarch.cfg and runs that writable
copy. With no SD it uses /tmp/retroarch instead of writing into /mnt/app.
