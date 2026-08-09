Audi MIB2 serial recovery capture

Device: /dev/cu.usbserial-BG03TXLM at 115200 baud
Date: 2026-08-04

Observed recovery:
- Removed /mnt/app/eso/hmi/lsd/jars/ra_mhi2q.jar
- Ran sync and verified the JAR was absent from the directory listing
- HMI subsequently reached: HMI STARTUP FINISHED [33117ms]
- Last observed heartbeat was at system uptime 00:00:38
- No subsequent Unrecoverable HMI Error or new boot banner was observed

Target-side dumps remain at:
- /mnt/ota/system/logs/error_1000073-001.zip (101069 bytes)
- /mnt/ota/system/core/j9.core.gz (147456 bytes)

The HU appeared to enter standby before those two binary files could be exported.
