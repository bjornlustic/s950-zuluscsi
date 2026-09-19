# ZuluSCSI Wi-Fi loader (SuperOS fork)

A fork of [ZuluSCSI-firmware](https://github.com/ZuluSCSI/ZuluSCSI-firmware)
for the ZuluSCSI Pico 2 W. It serves a vintage sampler its SCSI disk image
as stock firmware does, and additionally accepts **byte-range reads and
writes of that image over Wi-Fi while the sampler is running** - no card
swap, no MSC mode switch, no interruption of the SCSI bus.

That is the whole feature. It knows nothing about Akai formats: a host
patches byte ranges of an image id, which is why the same client drives an
S950, an S1000 or anything else on the bus. The Akai side of the story (a
browser page that turns audio into samples the sampler picks up seconds
later) is the separate S950 web editor repo.

## This is a fork, not upstream

- Base: upstream `main` at commit `6f6b113` ("Merge pull request #948 …",
  2026-09-06). Upstream is the `upstream` remote here; its own README is
  [README-ZuluSCSI-upstream.md](README-ZuluSCSI-upstream.md).
- Added: `src/SuperOS_loader.cpp` / `.h`, plus hooks in `src/ZuluSCSI.cpp`
  (main loop, Wi-Fi join), `src/ZuluSCSI_disk.cpp` and
  `lib/ZuluSCSI_platform_RP2MCU/ZuluSCSI_platform_network.cpp`. One commit,
  ~600 lines. Design notes: [SUPEROS.md](SUPEROS.md).
- **You are flashing a third-party build.** To get back to stock, flash an
  official ZuluSCSI release the same way you flashed this one; nothing here
  changes the card layout or the images on it.
- ZuluSCSI™ is a trademark of Rabbit Hole Computing™ and this fork is not
  endorsed by them. Report problems here, not to upstream.

## Licence

**GPLv3**, inherited from upstream (see [LICENSE](LICENSE)) - which is also
why the firmware ships here as source.

One exception: `host/zulu_udp.py` is host-side code written for this
project, shares nothing with upstream, and is **MIT**, so you can copy the
protocol client into your own tooling. The licence is stated in that file.

## Build and flash

```bash
pio run -e ZuluSCSI_Pico_2_DaynaPORT
# -> .pio/build/ZuluSCSI_Pico_2_DaynaPORT/firmware.uf2
# flash: in the USB console press 'u' then 'y'; the RP2350 volume appears;
#        copy firmware.uf2 onto it
```

The DaynaPORT env is the one with the Wi-Fi chip enabled; the loader rides
the same raw-frame path (the firmware has no lwIP, so the loader is its own
minimal ARP + ICMP + UDP responder).

## Card and config

In `zuluscsi.ini`, under `[SCSI]`:

    WiFiSSID = "your-network"
    WiFiPassword = "your-password"
    LoaderIP = "192.168.1.250"      ; optional, this is the default

An `NE6.img` must exist in the image directory, otherwise the firmware
never brings Wi-Fi up (the network device is what initialises it). Your
sampler's image (for the S950: `HD00_512.hda`) sits alongside it as usual.

Hardware specifics - board revision, the DB25 adapter, cabling to the
sampler - are deliberately not documented here, because wrong wiring can
damage a sampler. Follow ZuluSCSI's own hardware documentation.

## Protocol

UDP port 5150. Request `"SOS1" cmd:u8 flags:u8 seq:u16 id:u8 pad[3]
offset:u64 len:u32 data[len]`, reply `"SOSR" cmd:u8 status:u8 …`, all
little-endian. Commands: INFO=1, WRITE=2, READ=3, FLUSH=4, PING=5, STAT=6;
status 0 ok, 1 no such image, 2 not writable, 3 io error, 4 bad request,
5 busy. Canonical definition: [src/SuperOS_loader.h](src/SuperOS_loader.h).

```bash
python3 host/zulu_udp.py info  --id 0
python3 host/zulu_udp.py read  0 512 --id 0
python3 host/zulu_udp.py patch changes.akpatch --id 0
```

Timing matters for anyone writing their own client. The board applies
writes **only when the SCSI bus is free**, and a polling sampler grabs the
bus every few seconds: measured RTT is 44-149 ms while the S950 polls,
against 8 ms idle. A fixed short retransmit gives up inside a single
bus-busy window; `zulu_udp.py` backs off (0.30 s, x1.7, capped at 2 s) and
gives up on a 25 s per-datagram deadline instead of an attempt count. Chunk
1440 bytes (frames much above ~1510 B are dropped by the receive path),
window 16.

Measured 2026-09-08 on the author's board: 640-900 KB/s write, 750-950 KB/s
read, ping 6-20 ms with Wi-Fi power save off. Variance comes from SD-write
stalls; SD writes run on core 1 so the Wi-Fi chip keeps being serviced.

## Status

In daily use by the author on an Akai S950, and previously an S1000. It has
not been tested on any other host, board revision or sampler.
