# ZuluSCSI Wi-Fi loader (SuperOS fork)

A fork of [ZuluSCSI-firmware](https://github.com/ZuluSCSI/ZuluSCSI-firmware)
for the ZuluSCSI Pico 2 W. It serves the sampler its SCSI disk image as stock
firmware does, and also reads and writes byte ranges of that image over Wi-Fi
while the sampler runs.

Install: **[INSTALL.md](INSTALL.md)**. Web editor that uses it:
<https://github.com/bjornlustic/s950-web-editor>.

## Fork

- Base: upstream `main` at `6f6b113`. Upstream README:
  [README-ZuluSCSI-upstream.md](README-ZuluSCSI-upstream.md).
- Added: `src/SuperOS_loader.cpp` / `.h`, hooks in `src/ZuluSCSI.cpp`,
  `src/ZuluSCSI_disk.cpp` and
  `lib/ZuluSCSI_platform_RP2MCU/ZuluSCSI_platform_network.cpp`, and
  `host/zulu_udp.py`.
- The loader is built only for `ZuluSCSI_Pico_2_DaynaPORT`; other targets
  build as stock.
- To go back to stock, flash an official ZuluSCSI release.
- ZuluSCSI is a trademark of Rabbit Hole Computing, who do not endorse this
  fork. Report problems here, not upstream.

## Build

```bash
pio run -e ZuluSCSI_Pico_2_DaynaPORT
# -> .pio/build/ZuluSCSI_Pico_2_DaynaPORT/firmware.uf2
```

## Protocol

UDP port 5150, little-endian. Request `"SOS1" cmd:u8 flags:u8 seq:u16 id:u8
pad[3] offset:u64 len:u32 data[len]`, reply `"SOSR" cmd:u8 status:u8 ...`.
Commands: INFO=1, WRITE=2, READ=3, FLUSH=4, PING=5, STAT=6. Status: 0 ok,
1 no such image, 2 not writable, 3 io error, 4 bad request, 5 busy.
Definition: [src/SuperOS_loader.h](src/SuperOS_loader.h). Writes are applied
only while the SCSI bus is free, so clients should back off rather than
retry on a fixed short timer.

```bash
python3 host/zulu_udp.py ping
python3 host/zulu_udp.py info --id 0
python3 host/zulu_udp.py read 0 512 --id 0
```

## Licence

GPLv3, inherited from upstream ([LICENSE](LICENSE)). `host/zulu_udp.py` is
MIT (stated in the file).
