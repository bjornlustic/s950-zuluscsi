# Install

For Akai S950 owners running SuperOS-950 5.0. Result: a ZuluSCSI that is
the S950's SCSI disk and also accepts writes to that disk over Wi-Fi.

## 1. Hardware

1. A ZuluSCSI Pico OSHW board fitted with a **Raspberry Pi Pico 2 W**.
   The loader is built only for this combination (firmware target
   `ZuluSCSI_Pico_2_DaynaPORT`). A Pico 2 without the W, a Pico W, a
   Blaster, an RP2040 or a V1.x board will not run it.
2. A microSD card.
3. An S950 with a SCSI interface (IB-109 or Rephlux). Cabling and
   termination: follow ZuluSCSI's own hardware documentation.
4. A 2.4 GHz Wi-Fi network. The Pico 2 W radio has no 5 GHz.

## 2. Get the firmware

There is no GitHub release. Firmware comes from the build server
(GitHub Actions) or from your own build.

**Download** (needs a free GitHub account):

1. Open <https://github.com/bjornlustic/s950-zuluscsi/actions>.
2. Click the newest run of "Build ZuluSCSI firmware" on branch `superos`
   with a green tick.
3. Under **Artifacts**, download **ZuluSCSI UF2s** and unzip it.
4. Use the file named `ZuluSCSI_Pico_2_DaynaPORT_<date>_<version>.uf2`.
   Ignore the others.

Artifacts expire after a while. If the list is empty, build it yourself.

**Build** (needs Python and PlatformIO):

```bash
git clone -b superos https://github.com/bjornlustic/s950-zuluscsi.git
cd s950-zuluscsi
pip install -U platformio
pio run -e ZuluSCSI_Pico_2_DaynaPORT
# -> .pio/build/ZuluSCSI_Pico_2_DaynaPORT/firmware.uf2
```

## 3. Flash

1. Unplug the board from the sampler.
2. Hold the **BOOTSEL** button on the Pico 2 W and plug it into your
   computer with USB. Release the button.
3. A drive named `RP2350` appears.
4. Copy the `.uf2` file onto it. The board reboots when the copy ends.

To go back to stock, flash an official ZuluSCSI release the same way.

## 4. Prepare the SD card

Put these in the root of the card:

1. `zuluscsi.ini` with:

   ```ini
   [SCSI]
   WiFiSSID = "your-network"
   WiFiPassword = "your-password"
   LoaderIP = "192.168.1.250"
   ```

   `LoaderIP` must be a free address on your home network (same first
   three numbers as your computer's address, and not used by any other
   device). The board does not use DHCP: this is its only address.
   `192.168.1.250` is the default if you leave the line out.
2. An empty file named `NE6.img`. Without it the board never starts
   Wi-Fi.

   ```bash
   touch /Volumes/<CARD>/NE6.img        # macOS; any empty file will do
   ```
3. The S950's disk, at SCSI ID 0. The web editor's install guide builds
   it from the SuperOS-950 download and puts it on the card as
   `S950/HD00_512.hda` with `Dir1 = "S950"` added under `[SCSI]`. Do that
   step there, because the editor keeps a matching copy on your computer.

## 5. Check it works

1. Put the card in the board, connect the board to the S950, power up.
2. From a computer on the same network:

   ```bash
   ping 192.168.1.250
   python3 host/zulu_udp.py ping
   python3 host/zulu_udp.py info --id 0
   ```

   `host/zulu_udp.py` is in this repo; the web editor has the same file
   as `tools/zulu_udp.py`. Use `--host <LoaderIP>` if you changed the
   address. `info` should show `HD00_512.hda`.
3. If nothing answers, read `zululog.txt` on the card. A working board
   logs `SuperOS loader listening on <LoaderIP> UDP port 5150`.

## 6. Troubleshooting

| Symptom | Fix |
|---|---|
| Log: "no SCSI ID assigned to a network device" | `NE6.img` is missing from the card. |
| Log: "no WiFi SSID specified" | `WiFiSSID` is missing, or not under `[SCSI]`. |
| Log: "authentication failure" | Wrong `WiFiPassword`. On a WPA3-only network add `WiFiSecurity = WPA3`. |
| Log: "SSID ... not found" | Network is 5 GHz only, out of range, or the name is misspelled. |
| Joined, but ping fails | `LoaderIP` is on a different subnet from your computer, or another device has that address. |
| `info --id 0` says no such image | The S950 image is not at ID 0, or its folder is not listed as `Dir1`. |
| Sampler does not see the disk | Image must be ID 0 (`HD00_512.hda`). Check cabling and termination. |

## Next

Install the web editor:
<https://github.com/bjornlustic/s950-web-editor/blob/main/docs/INSTALL.md>
