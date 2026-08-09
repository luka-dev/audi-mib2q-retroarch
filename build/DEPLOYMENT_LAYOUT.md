# Deployment outputs

`./build.sh` produces two directly deployable filesystem roots:

- `mnt_app/` → copy its contents to `/mnt/app` during an explicit maintenance deployment.
- `sd_card/` → copy its contents to the root of the FAT32 card mounted as `/fs/sda0`.

The app image is the immutable factory layer: frontend, cores, runtime libraries,
Audi/Ozone UI assets and fonts, controller mappings, rumble profiles, core-info
seed, factory config, launcher, and the HMI JAR.

The SD is the replaceable user layer: live config, BIOS, games, saves, states,
playlists, box art, databases, cheats, logs, screenshots and optional user data.
On first launch with a blank card, `ra.sh` seeds a working config from the
factory template. Explicit configuration saves and save-on-exit therefore write
only to the current SD. Replacing the card restores the known-good defaults on
the next launch.

## Project inputs

The reproducible project is not only `src`, `cores-src`, `java_patch` and
`build`. It also needs `build.sh`, `pkg` (factory config and external resources),
`lsd_patch` (the verified Java builder/current JAR), and `testcore`. The current
games and BIOS are sourced from `build/macos-local`; `build/mnt_app` and
`build/sd_card` are generated deployment outputs. Legacy diagnostic material in
`artifacts` is retained for investigation but is no longer a build dependency.
