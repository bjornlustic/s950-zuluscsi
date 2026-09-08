// SuperOS loader: write into the SCSI disk images over Wi-Fi while the sampler runs.
//
// ZuluSCSI builds without lwIP (Wi-Fi frames go raw to the DaynaPORT emulation), so this
// is a minimal ARP + ICMP echo + UDP responder on the same raw frame path. A host sends
// UDP datagrams to port SUPEROS_LOADER_PORT at the address from zuluscsi.ini
// ([SCSI] LoaderIP, default 192.168.1.250); each one reads or writes a byte range of an
// image file. Writes are applied when the SCSI bus is free and the read prefetch cache
// is invalidated, so the host sees the new data on its next directory read.
//
// Datagram layout, little-endian:
//   request  "SOS1" cmd:u8 flags:u8 seq:u16 id:u8 pad:u8[3] offset:u64 len:u32 data[len]
//   reply    "SOSR" cmd:u8 status:u8 seq:u16 id:u8 pad:u8[3] offset:u64 len:u32 data[len]
//   cmd 1 INFO  -> data = size:u64 blocksize:u32 filename[64]
//   cmd 2 WRITE -> data written at offset
//   cmd 3 READ  -> data read from offset (len <= 1400)
//   cmd 4 FLUSH -> flush image, invalidate prefetch
//   cmd 5 PING
//   cmd 6 STAT  -> data = last_read_lba:u32 reads:u32 writes:u32 (host reads on this id)
//   status 0 ok, 1 no such image, 2 not writable, 3 io error, 4 bad request, 5 busy
#pragma once
#include <stdint.h>
#include <stddef.h>

#define SUPEROS_LOADER_PORT 5150

void superos_loader_init();
// Called from the raw ethernet receive callback. Returns true if the frame was consumed.
bool superos_loader_rx(const uint8_t *frame, size_t len);
// Called from the main loop (core 0); parses requests, stages writes for the core 1 SD
// writer, and lends the SD card to core 1 when the SCSI bus is free.
void superos_loader_poll();
// True while core 1 owns the SD card / SdFat. zuluscsi_main_loop() must then skip every
// SD user (SCSI target, log save, hotplug, image swap, console) until it returns false.
bool superos_loader_sd_lent();
// Called from the SCSI READ path so the host can see which blocks the sampler reads.
void superos_loader_note_read(uint8_t scsi_id, uint32_t lba, uint32_t blocks);
