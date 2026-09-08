// SuperOS loader: see SuperOS_loader.h.
//
// Threading model (see SUPEROS.md, "Core 1 SD writer"):
//   core 0: cyw43 driver (rx callback, platform_network_send, cyw43_arch_poll), protocol
//           parsing, WRITE staging into RAM slots, replies, READ/INFO (when the queue is
//           drained), prefetch invalidation, all logging.
//   core 1: seek/write/flush of the image file for queued slots. Never touches cyw43.
// The SD card / SdFat is single-owner: core 0 lends it to core 1 (g_sd_lent) only while
// the SCSI bus is free, and zuluscsi_main_loop() runs no SD-touching code while lent.
#include "SuperOS_loader.h"
#include "ZuluSCSI_config.h"
#include "ZuluSCSI_log.h"
#include "ZuluSCSI_disk.h"
#include "ZuluSCSI_platform_network.h"
#include <minIni.h>
#include <scsi.h>
#include <string.h>
#include <stdio.h>
#include <Arduino.h>

#ifdef ZULUSCSI_NETWORK
extern "C" {
#include <cyw43.h>
#include <pico/cyw43_arch.h>
#include <pico/multicore.h>
}

#define MAX_FRAME 1600
#define MAX_DATA 1472

struct __attribute__((packed)) loader_hdr {
    char magic[4];
    uint8_t cmd;
    uint8_t flags_or_status;
    uint16_t seq;
    uint8_t id;
    uint8_t pad[3];
    uint64_t offset;
    uint32_t len;
};

enum { CMD_INFO = 1, CMD_WRITE = 2, CMD_READ = 3, CMD_FLUSH = 4, CMD_PING = 5, CMD_STAT = 6 };
enum { ST_OK = 0, ST_NO_IMAGE = 1, ST_READ_ONLY = 2, ST_IO = 3, ST_BAD = 4, ST_BUSY = 5 };

static uint8_t g_ip[4] = {192, 168, 1, 250};
static bool g_enabled = false;

// Ring of pending request frames: copied out of the receive callback, executed in poll().
#define PENDING_RING 24
static uint8_t g_pending[PENDING_RING][MAX_FRAME];
static size_t g_pending_len[PENDING_RING];
static volatile uint8_t g_pend_head = 0, g_pend_tail = 0;
static uint8_t g_reply[MAX_FRAME];
static uint32_t g_stats_rx = 0, g_stats_writes = 0;
static uint32_t g_last_read_lba[S2S_MAX_TARGETS], g_read_count[S2S_MAX_TARGETS];

// ---------------------------------------------------------------------------
// Core 0 -> core 1 job queue (single producer, single consumer, lock free).
//
// Slots hold either a coalesced, contiguous WRITE (up to SLOT_SIZE bytes, staged by
// core 0 while the slot is still unpublished) or a FLUSH marker. Three indices:
//   g_q_head  core 0 writes: next slot to publish (the "open" slot being staged)
//   g_q_tail  core 1 writes: next slot to execute
//   g_q_ack   core 0 writes: next finished slot to reap (reply / prefetch invalidate)
// Order of ownership for a slot: core 0 (staging) -> core 1 (executing) -> core 0 (reaping).
// Publication and completion use __dmb() so data is visible before the index advances.
// The slot at g_q_head is only usable when (g_q_head + 1) % SLOTS != g_q_ack.
// ---------------------------------------------------------------------------
#define SLOT_SIZE 8192
#define SLOTS 8                          // 64 KB of queued writes (RAM budget: heap is what .bss leaves)
enum { JOB_WRITE = 1, JOB_FLUSH = 2 };

struct job_t {
    uint8_t type;
    uint8_t id;
    uint8_t status;                      // written by core 1
    volatile uint8_t done;               // written by core 1
    uint32_t len;
    uint64_t off;
    // reply routing for FLUSH (core 0 sends the reply after core 1 completes it)
    bool want_reply;
    uint8_t dst_mac[6];
    uint8_t dst_ip[4];
    uint16_t dst_port;
    uint16_t seq;
};
static job_t g_jobs[SLOTS];
static uint8_t g_slot_data[SLOTS][SLOT_SIZE];
static volatile uint8_t g_q_head = 0, g_q_tail = 0, g_q_ack = 0;

// Open (unpublished) slot staging state, core 0 only.
static uint32_t g_open_len = 0;
static uint64_t g_open_off = 0;
static int g_open_id = -1;
static uint32_t g_last_write_time = 0;
static bool g_unflushed = false;         // a WRITE was published after the last FLUSH job
static int g_unflushed_id = -1;

// SD ownership handoff.
//   g_sd_lent  core 0 only: the SD/SdFat currently belongs to core 1; the main loop
//              must not run any SD-touching code (SCSI, log save, hotplug, console).
//   g_c1_busy  set by core 0 before kicking core 1, cleared by core 1 when it returns.
static bool g_sd_lent = false;
static volatile uint8_t g_c1_busy = 0;
static uint32_t g_dirty_mask = 0;        // ids written since the last prefetch invalidate (core 0)
static bool g_io_error = false;          // latched WRITE failure, reported on the next FLUSH reply
static uint32_t g_c1_runs = 0;
static uint32_t g_lent_since = 0;
static bool g_lent_warned = false;

// Per-image facts cached by core 0 while it owns the SD, so WRITE validation never
// touches the FsFile object while core 1 may be using it.
static bool g_img_open[S2S_MAX_TARGETS];
static bool g_img_writable[S2S_MAX_TARGETS];
static uint64_t g_img_size[S2S_MAX_TARGETS];

static inline uint8_t q_next(uint8_t i) { return (uint8_t)((i + 1) % SLOTS); }
static inline bool q_open_slot_free() { return q_next(g_q_head) != g_q_ack; }
static inline bool q_drained() { return g_open_len == 0 && g_q_head == g_q_tail && g_q_head == g_q_ack; }

static void refresh_image_facts()
{
    for (int i = 0; i < S2S_MAX_TARGETS; i++)
    {
        image_config_t &img = scsiDiskGetImageConfig(i);
        g_img_open[i] = img.file.isOpen();
        g_img_writable[i] = g_img_open[i] && img.file.isWritable();
        g_img_size[i] = g_img_open[i] ? img.file.size() : 0;
    }
}

// Runs on core 1 via the platform's core1_handler dispatcher (holds g_core1_mutex while
// running, so platform_write_romdrive() waits for it instead of disabling XIP under it).
// No cyw43, no logmsg. Returns as soon as the queue is empty; core 0 re-kicks it.
static void core1_worker()
{
    while (true)
    {
        uint8_t t = g_q_tail;
        if (t == g_q_head) break;
        __dmb();
        job_t &j = g_jobs[t];
        image_config_t &img = scsiDiskGetImageConfig(j.id);
        bool ok;
        if (j.type == JOB_WRITE)
        {
            ok = img.file.isOpen() && img.file.seek(j.off) &&
                 img.file.write(g_slot_data[t], j.len) == (ssize_t)j.len;
        }
        else
        {
            if (img.file.isOpen()) img.file.flush();
            ok = true;
        }
        j.status = ok ? ST_OK : ST_IO;
        __dmb();
        j.done = 1;
        __dmb();
        g_q_tail = q_next(t);
    }
    __dmb();
    g_c1_busy = 0;
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v & 0xff; }

static uint16_t csum(const uint8_t *p, size_t n, uint32_t acc = 0)
{
    while (n > 1) { acc += rd16(p); p += 2; n -= 2; }
    if (n) acc += (uint32_t)p[0] << 8;
    while (acc >> 16) acc = (acc & 0xffff) + (acc >> 16);
    return (uint16_t)~acc;
}

static void send_frame(uint8_t *frame, size_t len)
{
    if (len < 60) { memset(frame + len, 0, 60 - len); len = 60; }
    platform_network_send(frame, len);
}

// Build an IPv4 header + payload framing towards `dst_mac`/`dst_ip`; returns the frame length.
static size_t build_ip(uint8_t *frame, const uint8_t *dst_mac, const uint8_t *dst_ip, uint8_t proto, size_t payload_len)
{
    memcpy(frame, dst_mac, 6);
    memcpy(frame + 6, cyw43_state.mac, 6);
    frame[12] = 0x08; frame[13] = 0x00;
    uint8_t *ip = frame + 14;
    size_t total = 20 + payload_len;
    ip[0] = 0x45; ip[1] = 0;
    wr16(ip + 2, total);
    wr16(ip + 4, 0); wr16(ip + 6, 0x4000);
    ip[8] = 64; ip[9] = proto;
    wr16(ip + 10, 0);
    memcpy(ip + 12, g_ip, 4);
    memcpy(ip + 16, dst_ip, 4);
    wr16(ip + 10, csum(ip, 20));
    return 14 + total;
}

static void handle_arp(const uint8_t *f, size_t len)
{
    const uint8_t *a = f + 14;
    if (len < 42 || rd16(a) != 1 || rd16(a + 2) != 0x0800 || a[4] != 6 || a[5] != 4) return;
    if (rd16(a + 6) != 1 || memcmp(a + 24, g_ip, 4) != 0) return;   // request for our IP
    uint8_t r[60];
    memcpy(r, a + 8, 6); memcpy(r + 6, cyw43_state.mac, 6);
    r[12] = 0x08; r[13] = 0x06;
    uint8_t *p = r + 14;
    wr16(p, 1); wr16(p + 2, 0x0800); p[4] = 6; p[5] = 4; wr16(p + 6, 2);
    memcpy(p + 8, cyw43_state.mac, 6); memcpy(p + 14, g_ip, 4);
    memcpy(p + 18, a + 8, 6); memcpy(p + 24, a + 14, 4);
    send_frame(r, 42);
}

static void handle_icmp(const uint8_t *f, size_t len)
{
    const uint8_t *ip = f + 14;
    size_t ihl = (ip[0] & 0x0f) * 4;
    size_t iplen = rd16(ip + 2);
    if (iplen < ihl + 8 || 14 + iplen > len) return;
    const uint8_t *icmp = ip + ihl;
    if (icmp[0] != 8) return;                                        // echo request
    size_t plen = iplen - ihl;
    if (plen > MAX_DATA) return;
    size_t n = build_ip(g_reply, f + 6, ip + 12, 1, plen);
    uint8_t *out = g_reply + 14 + 20;
    memcpy(out, icmp, plen);
    out[0] = 0; wr16(out + 2, 0);
    wr16(out + 2, csum(out, plen));
    send_frame(g_reply, n);
}

static void reply_to(const uint8_t *dst_mac, const uint8_t *dst_ip, uint16_t dst_port,
                     uint8_t cmd, uint16_t seq, uint8_t id, uint64_t offset,
                     uint8_t status, const void *data, uint32_t dlen)
{
    size_t udp_len = 8 + sizeof(loader_hdr) + dlen;
    size_t n = build_ip(g_reply, dst_mac, dst_ip, 17, udp_len);
    uint8_t *u = g_reply + 14 + 20;
    wr16(u, SUPEROS_LOADER_PORT); wr16(u + 2, dst_port); wr16(u + 4, udp_len); wr16(u + 6, 0);
    loader_hdr *h = (loader_hdr *)(u + 8);
    memcpy(h->magic, "SOSR", 4);
    h->cmd = cmd; h->flags_or_status = status; h->seq = seq; h->id = id;
    memset(h->pad, 0, 3); h->offset = offset; h->len = dlen;
    if (dlen) memcpy(u + 8 + sizeof(loader_hdr), data, dlen);
    send_frame(g_reply, n);
}

static void reply(const uint8_t *req_frame, const loader_hdr *req, uint8_t status, const void *data, uint32_t dlen)
{
    const uint8_t *ip = req_frame + 14;
    size_t ihl = (ip[0] & 0x0f) * 4;
    const uint8_t *udp = ip + ihl;
    reply_to(req_frame + 6, ip + 12, rd16(udp), req->cmd, req->seq, req->id, req->offset, status, data, dlen);
}

// Publish the open slot as a WRITE job. Caller guarantees the slot is usable.
static void publish_open()
{
    if (g_open_len == 0) return;
    job_t &j = g_jobs[g_q_head];
    j.type = JOB_WRITE; j.id = (uint8_t)g_open_id; j.status = ST_OK; j.done = 0;
    j.len = g_open_len; j.off = g_open_off; j.want_reply = false;
    __dmb();
    g_q_head = q_next(g_q_head);
    g_open_len = 0;
    g_unflushed = true;
    g_unflushed_id = g_open_id;
    g_stats_writes++;
}

// Queue a FLUSH job. Caller guarantees a usable slot.
static void enqueue_flush(uint8_t id, const uint8_t *req_frame, const loader_hdr *req)
{
    job_t &j = g_jobs[g_q_head];
    j.type = JOB_FLUSH; j.id = id; j.status = ST_OK; j.done = 0; j.len = 0; j.off = 0;
    j.want_reply = req_frame != NULL;
    if (req_frame)
    {
        const uint8_t *ip = req_frame + 14;
        size_t ihl = (ip[0] & 0x0f) * 4;
        memcpy(j.dst_mac, req_frame + 6, 6);
        memcpy(j.dst_ip, ip + 12, 4);
        j.dst_port = rd16(ip + ihl);
        j.seq = req->seq;
    }
    __dmb();
    g_q_head = q_next(g_q_head);
    g_unflushed = false;
}

// Reap completed jobs (core 0). Sends FLUSH replies, latches WRITE errors, frees slots.
static void reap()
{
    while (g_q_ack != g_q_tail)
    {
        __dmb();
        job_t &j = g_jobs[g_q_ack];
        if (j.type == JOB_WRITE)
        {
            g_dirty_mask |= 1u << j.id;
            if (j.status != ST_OK) g_io_error = true;
        }
        else if (j.want_reply)
        {
            uint8_t st = g_io_error ? ST_IO : j.status;
            g_io_error = false;
            reply_to(j.dst_mac, j.dst_ip, j.dst_port, CMD_FLUSH, j.seq, j.id, 0, st, NULL, 0);
        }
        __dmb();
        g_q_ack = q_next(g_q_ack);
    }
}

// Execute one request frame on core 0. Returns false if it must be retried later
// (queue full, or READ/INFO waiting for the SD to be drained and returned to core 0).
static bool execute(const uint8_t *f, size_t len)
{
    const uint8_t *ip = f + 14;
    size_t ihl = (ip[0] & 0x0f) * 4;
    const uint8_t *udp = ip + ihl;
    size_t udp_len = rd16(udp + 4);
    if (udp_len < 8 + sizeof(loader_hdr) || 14 + ihl + udp_len > len) return true;
    const loader_hdr *h = (const loader_hdr *)(udp + 8);
    const uint8_t *data = udp + 8 + sizeof(loader_hdr);
    if (memcmp(h->magic, "SOS1", 4) != 0) return true;
    if (h->len > udp_len - 8 - sizeof(loader_hdr) && h->cmd == CMD_WRITE) { reply(f, h, ST_BAD, NULL, 0); return true; }
    g_stats_rx++;

    if (h->cmd == CMD_PING) { reply(f, h, ST_OK, NULL, 0); return true; }
    if (h->id >= S2S_MAX_TARGETS) { reply(f, h, ST_NO_IMAGE, NULL, 0); return true; }
    if (h->cmd == CMD_STAT) {
        uint32_t st[3] = { g_last_read_lba[h->id], g_read_count[h->id], g_stats_writes };
        reply(f, h, ST_OK, st, sizeof(st));
        return true;
    }
    if (!g_img_open[h->id]) { reply(f, h, ST_NO_IMAGE, NULL, 0); return true; }

    switch (h->cmd)
    {
    case CMD_INFO:
    case CMD_READ: {
        // Must observe everything written so far: wait until the queue is drained and
        // the SD is back with core 0, then do the I/O here.
        if (g_open_len > 0 && q_open_slot_free()) publish_open();
        if (g_sd_lent || !q_drained() || scsiDev.phase != BUS_FREE) return false;
        image_config_t &img = scsiDiskGetImageConfig(h->id);
        if (h->cmd == CMD_INFO)
        {
            uint8_t info[76];
            uint64_t size = img.file.size();
            memcpy(info, &size, 8);
            uint32_t bs = img.bytesPerSector;
            memcpy(info + 8, &bs, 4);
            memset(info + 12, 0, 64);
            img.file.getFilename((char *)info + 12, 64);
            reply(f, h, ST_OK, info, sizeof(info));
            return true;
        }
        uint32_t n = h->len > MAX_DATA ? MAX_DATA : h->len;
        static uint8_t buf[MAX_DATA];
        if (!img.file.seek(h->offset) || img.file.read(buf, n) != (ssize_t)n) { reply(f, h, ST_IO, NULL, 0); return true; }
        reply(f, h, ST_OK, buf, n);
        return true;
    }
    case CMD_WRITE: {
        if (!g_img_writable[h->id]) { reply(f, h, ST_READ_ONLY, NULL, 0); return true; }
        if (h->len > SLOT_SIZE || h->offset + h->len > g_img_size[h->id]) { reply(f, h, ST_BAD, NULL, 0); return true; }
        bool contiguous = g_open_len > 0 && g_open_id == h->id && g_open_off + g_open_len == h->offset;
        if (g_open_len > 0 && (!contiguous || g_open_len + h->len > SLOT_SIZE))
        {
            if (!q_open_slot_free()) return false;               // backpressure: retry next poll
            publish_open();
        }
        if (g_open_len == 0)
        {
            if (!q_open_slot_free()) return false;
            g_open_off = h->offset; g_open_id = h->id;
        }
        memcpy(g_slot_data[g_q_head] + g_open_len, data, h->len);
        g_open_len += h->len;
        g_last_write_time = millis();
        if (g_open_len >= SLOT_SIZE - MAX_DATA && q_open_slot_free()) publish_open();
        reply(f, h, ST_OK, NULL, 0);                              // acked once queued in RAM
        return true;
    }
    case CMD_FLUSH:
        // Needs up to two slots: the open WRITE and the FLUSH marker itself.
        if (g_open_len > 0)
        {
            if (!q_open_slot_free()) return false;
            publish_open();
        }
        if (!q_open_slot_free()) return false;
        enqueue_flush(h->id, f, h);                              // replied by reap() when done
        return true;
    default:
        reply(f, h, ST_BAD, NULL, 0);
        return true;
    }
}

void superos_loader_init()
{
    char tmp[32];
    ini_gets("SCSI", "LoaderIP", "192.168.1.250", tmp, sizeof(tmp), CONFIGFILE);
    unsigned a, b, c, d;
    if (sscanf(tmp, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) { g_ip[0] = a; g_ip[1] = b; g_ip[2] = c; g_ip[3] = d; }
    refresh_image_facts();
    g_enabled = true;
    logmsg("SuperOS loader listening on ", (int)g_ip[0], ".", (int)g_ip[1], ".", (int)g_ip[2], ".", (int)g_ip[3],
           " UDP port ", (int)SUPEROS_LOADER_PORT, ", core 1 SD writer, queue ", (int)(SLOTS * SLOT_SIZE / 1024), " KB");
}

bool superos_loader_rx(const uint8_t *f, size_t len)
{
    if (!g_enabled || len < 42 || len > MAX_FRAME) return false;
    bool bcast = f[0] == 0xff;
    bool mine = memcmp(f, cyw43_state.mac, 6) == 0;
    if (!bcast && !mine) return false;
    uint16_t type = rd16(f + 12);
    if (type == 0x0806) { handle_arp(f, len); return true; }
    if (type != 0x0800 || !mine) return false;
    const uint8_t *ip = f + 14;
    if ((ip[0] >> 4) != 4 || memcmp(ip + 16, g_ip, 4) != 0) return false;
    if (ip[9] == 1) { handle_icmp(f, len); return true; }
    if (ip[9] != 17) return false;
    size_t ihl = (ip[0] & 0x0f) * 4;
    if (rd16(ip + ihl + 2) != SUPEROS_LOADER_PORT) return false;
    uint8_t next = (g_pend_head + 1) % PENDING_RING;
    if (next == g_pend_tail) return true;                             // ring full, host retries
    memcpy(g_pending[g_pend_head], f, len);
    g_pending_len[g_pend_head] = len;
    g_pend_head = next;
    return true;
}

bool superos_loader_sd_lent()
{
    return g_sd_lent;
}

static bool g_pm_done = false;

void superos_loader_poll()
{
    // Wi-Fi power save (driver default PM2) sleeps the chip 200 ms after each packet and
    // costs 100+ ms per round trip. Turn it off once the link is up.
    if (!g_pm_done && cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_JOIN)
    {
        cyw43_wifi_pm(&cyw43_state, CYW43_NONE_PM);
        g_pm_done = true;
        logmsg("SuperOS loader: Wi-Fi power save disabled");
    }
    if (!g_enabled) return;

    // 1. Take the SD back from core 1 once it is idle. Read g_c1_busy BEFORE reaping so
    //    that "idle" implies g_q_tail is final (core 1 writes tail, dmb, then busy = 0).
    bool c1_idle = g_c1_busy == 0;
    __dmb();
    reap();
    if (g_sd_lent && c1_idle)
    {
        for (int i = 0; i < S2S_MAX_TARGETS; i++)
            if (g_dirty_mask & (1u << i)) scsiDiskPrefetchInvalidate(i);
        g_dirty_mask = 0;
        g_sd_lent = false;
        g_lent_warned = false;
    }
    else if (g_sd_lent && !g_lent_warned && (uint32_t)(millis() - g_lent_since) > 5000)
    {
        // Core 1 faulted or the SD hung: the main loop stays parked. Diagnostic only.
        logmsg("SuperOS loader: core 1 SD writer has not returned for 5 s (run ", (int)g_c1_runs, ")");
        g_lent_warned = true;
    }
    if (!g_sd_lent) refresh_image_facts();   // core 0 owns the FsFile objects now

    // 2. Execute pending requests (RAM staging and replies happen regardless of SCSI
    //    phase; SD I/O on core 0 only when the queue is drained and the bus is free).
    int n = 0;
    while (g_pend_tail != g_pend_head && n++ < PENDING_RING)
    {
        if (!execute(g_pending[g_pend_tail], g_pending_len[g_pend_tail])) break;
        g_pend_tail = (g_pend_tail + 1) % PENDING_RING;
        cyw43_arch_poll();
    }

    // 3. Idle handling: publish a partial slot and flush the image 200 ms after the last write.
    if ((uint32_t)(millis() - g_last_write_time) > 200)
    {
        if (g_open_len > 0 && q_open_slot_free()) publish_open();
        if (g_open_len == 0 && g_unflushed && q_open_slot_free()) enqueue_flush((uint8_t)g_unflushed_id, NULL, NULL);
    }

    // 4. Lend the SD to core 1 while the bus is free. zuluscsi_main_loop() checks
    //    superos_loader_sd_lent() and skips all SD users until step 1 takes it back.
    if (!g_sd_lent && g_q_head != g_q_tail && scsiDev.phase == BUS_FREE)
    {
        g_sd_lent = true;
        g_lent_since = millis();
        g_c1_busy = 1;
        g_c1_runs++;
        __dmb();
        multicore_fifo_push_blocking((uintptr_t)&core1_worker);
    }
}

void superos_loader_note_read(uint8_t scsi_id, uint32_t lba, uint32_t blocks)
{
    if (scsi_id < S2S_MAX_TARGETS) { g_last_read_lba[scsi_id] = lba; g_read_count[scsi_id]++; }
}

#else
void superos_loader_note_read(uint8_t, uint32_t, uint32_t) {}
void superos_loader_init() {}
bool superos_loader_rx(const uint8_t *, size_t) { return false; }
void superos_loader_poll() {}
bool superos_loader_sd_lent() { return false; }
#endif
