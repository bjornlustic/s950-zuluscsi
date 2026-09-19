#!/usr/bin/env python3
"""Host side of the SuperOS ZuluSCSI Wi-Fi loader (SuperOS-ZuluSCSIPicoSlim/src/SuperOS_loader.*).
Reads and writes byte ranges of an image on the running board over UDP, no card-reader
mode, no SCSI interruption. The board invalidates its read prefetch after every write.

  zulu_udp.py info  [--id 5]
  zulu_udp.py read  <offset> <len> [--id 5]
  zulu_udp.py patch <file.akpatch> [--id 0]      apply an AKPATCH1 file (tools/s950wifi.py)

Library: ZuluLoader(host).write_ranges(sid, [(offset, bytes), ...]).

The protocol is machine-agnostic (byte ranges of an image id), so the same
client drives an S950, an S1000 or anything else the board serves.

MIT licensed, unlike the firmware around it (GPLv3): this file is host-side
code written for this project and shares nothing with upstream ZuluSCSI, so
it can be copied into your own tooling freely.  A second copy lives in the
S950 web editor repo; keep them in step if the firmware protocol changes.
"""
import argparse, os, socket, struct, sys, time

HDR = '<4sBBHB3xQI'
HDR_LEN = struct.calcsize(HDR)
CMD_INFO, CMD_WRITE, CMD_READ, CMD_FLUSH, CMD_PING, CMD_STAT = 1, 2, 3, 4, 5, 6
STATUS = {0: 'ok', 1: 'no such image', 2: 'read only', 3: 'io error', 4: 'bad request', 5: 'busy'}
CHUNK = 1440
WINDOW = 16

# Retransmit policy.  The board applies writes only when the SCSI bus is
# free, and the S950 grabs the bus about every 4 s, so an in-flight
# datagram can go unanswered for seconds at a time.  Measured RTT with the
# sampler polling is 44-149 ms against 8 ms idle, so the old fixed 150 ms
# retransmit fired at or below the real RTT and 8 attempts burned out in
# ~1.2 s - far short of one bus-busy window, which is why pushes failed
# mid-transfer with 'no ack for write at N'.  Back off instead, and give up
# on a deadline rather than an attempt count.
RTO_BASE = 0.30            # first retransmit wait, above the worst RTT
RTO_MAX = 2.0              # cap
RTO_GROWTH = 1.7
DEADLINE = 25.0            # per datagram, rides out several bus grabs


def _rto(tries):
    return min(RTO_BASE * (RTO_GROWTH ** max(0, tries - 1)), RTO_MAX)


class ZuluLoader:
    def __init__(self, host='192.168.1.250', port=5150, timeout=1.0,
                 retries=8, deadline=DEADLINE):
        self.addr = (host, port)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(timeout)
        self.retries = retries          # still used by _xfer (single shot)
        self.deadline = deadline        # per-datagram give-up, seconds
        self.seq = 0

    def _xfer(self, cmd, sid=0, offset=0, length=0, data=b''):
        self.seq = (self.seq + 1) & 0xffff
        req = struct.pack(HDR, b'SOS1', cmd, 0, self.seq, sid, offset, length) + data
        for attempt in range(self.retries):
            self.sock.sendto(req, self.addr)
            try:
                while True:
                    pkt, _ = self.sock.recvfrom(2048)
                    if len(pkt) < HDR_LEN or pkt[:4] != b'SOSR':
                        continue
                    magic, rcmd, status, seq, rid, roff, rlen = struct.unpack(HDR, pkt[:HDR_LEN])
                    if seq != self.seq:
                        continue          # stale reply from a retried request
                    if status != 0:
                        raise RuntimeError('board replied %s (cmd %d, offset %d)' % (STATUS.get(status, status), cmd, offset))
                    return pkt[HDR_LEN:HDR_LEN + rlen]
            except socket.timeout:
                continue
        raise RuntimeError('no reply from %s:%d after %d attempts' % (self.addr[0], self.addr[1], self.retries))

    def ping(self):
        t = time.time(); self._xfer(CMD_PING); return time.time() - t

    def info(self, sid):
        d = self._xfer(CMD_INFO, sid)
        size, bs = struct.unpack('<QI', d[:12])
        return {'size': size, 'block_size': bs, 'filename': d[12:76].split(b'\0')[0].decode(errors='replace')}

    def read(self, sid, offset, length, progress=None):
        """Windowed read: same sliding window as write().

        The one-datagram-at-a-time version this replaced ran at the round
        trip time, ~99 KB/s on the S950's link, which made the read-back
        verification six times more expensive than the write it checks.
        With the window it tracks the write path.
        """
        chunks = [(offset + i, min(1400, length - i))
                  for i in range(0, length, 1400)]
        out = [None] * len(chunks)
        inflight = {}       # seq -> [index, last_sent, tries, first_sent]
        nxt = 0
        done = 0
        while done < len(chunks):
            while nxt < len(chunks) and len(inflight) < WINDOW:
                self.seq = (self.seq + 1) & 0xffff
                off, n = chunks[nxt]
                self.sock.sendto(struct.pack(HDR, b'SOS1', CMD_READ, 0,
                                             self.seq, sid, off, n), self.addr)
                now0 = time.time()
                inflight[self.seq] = [nxt, now0, 1, now0]
                nxt += 1
            try:
                pkt, _ = self.sock.recvfrom(2048)
            except socket.timeout:
                pkt = None
            if pkt and len(pkt) >= HDR_LEN and pkt[:4] == b'SOSR':
                _, _rcmd, status, seq, _rid, roff, rlen = struct.unpack(
                    HDR, pkt[:HDR_LEN])
                st = inflight.pop(seq, None)
                if st is not None:
                    if status != 0:
                        raise RuntimeError('board replied %s (read at %d)'
                                           % (STATUS.get(status, status), roff))
                    got = pkt[HDR_LEN:HDR_LEN + rlen]
                    if len(got) != chunks[st[0]][1]:
                        raise RuntimeError(
                            'short read at %d: %d of %d bytes'
                            % (roff, len(got), chunks[st[0]][1]))
                    out[st[0]] = got
                    done += 1
                    if progress:
                        progress(done * 1400)
            now = time.time()
            for seq, st in list(inflight.items()):
                if now - st[1] > _rto(st[2]):
                    if now - st[3] > self.deadline:
                        raise RuntimeError(
                            'no reply for read at %d after %.0f s (%d '
                            'attempts)' % (chunks[st[0]][0], now - st[3],
                                           st[2]))
                    del inflight[seq]
                    self.seq = (self.seq + 1) & 0xffff
                    off, n = chunks[st[0]]
                    self.sock.sendto(struct.pack(HDR, b'SOS1', CMD_READ, 0,
                                                 self.seq, sid, off, n),
                                     self.addr)
                    inflight[self.seq] = [st[0], now, st[2] + 1, st[3]]
        return b''.join(out)

    def write(self, sid, offset, data, progress=None):
        # Sliding window: up to WINDOW datagrams in flight, each acked by seq.
        chunks = [(offset + i, data[i:i + CHUNK]) for i in range(0, len(data), CHUNK)]
        inflight = {}      # seq -> [index, last_sent, tries, first_sent]
        nxt = 0
        done = 0
        while done < len(chunks):
            while nxt < len(chunks) and len(inflight) < WINDOW:
                self.seq = (self.seq + 1) & 0xffff
                off, d = chunks[nxt]
                self.sock.sendto(struct.pack(HDR, b'SOS1', CMD_WRITE, 0, self.seq, sid, off, len(d)) + d, self.addr)
                now0 = time.time()
                inflight[self.seq] = [nxt, now0, 1, now0]
                nxt += 1
            try:
                pkt, _ = self.sock.recvfrom(2048)
            except socket.timeout:
                pkt = None
            if pkt and len(pkt) >= HDR_LEN and pkt[:4] == b'SOSR':
                magic, rcmd, status, seq, rid, roff, rlen = struct.unpack(HDR, pkt[:HDR_LEN])
                if seq in inflight:
                    if status != 0:
                        raise RuntimeError('board replied %s (write at %d)' % (STATUS.get(status, status), roff))
                    del inflight[seq]
                    done += 1
                    if progress:
                        progress(min(len(data), done * CHUNK))
            now = time.time()
            for seq, st in list(inflight.items()):
                if now - st[1] > _rto(st[2]):
                    if now - st[3] > self.deadline:
                        raise RuntimeError(
                            'no ack for write at %d after %.0f s (%d '
                            'attempts)' % (chunks[st[0]][0], now - st[3],
                                           st[2]))
                    off, d = chunks[st[0]]
                    self.sock.sendto(struct.pack(HDR, b'SOS1', CMD_WRITE, 0, seq, sid, off, len(d)) + d, self.addr)
                    st[1] = now; st[2] += 1

    def stat(self, sid):
        d = self._xfer(CMD_STAT, sid)
        last_lba, reads, writes = struct.unpack('<III', d[:12])
        return {'last_lba': last_lba, 'reads': reads, 'writes': writes}

    def flush(self, sid):
        self._xfer(CMD_FLUSH, sid)

    def write_ranges(self, sid, writes, progress=None):
        done = 0
        for off, data in writes:
            self.write(sid, off, data)
            done += len(data)
            if progress:
                progress(done)
        self.flush(sid)
        for off, data in writes:
            if self.read(sid, off, len(data)) != data:
                raise RuntimeError('verify failed at offset %d' % off)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cmd', choices=['ping', 'info', 'read', 'patch', 'stat'])
    ap.add_argument('args', nargs='*')
    ap.add_argument('--host', default=os.environ.get('ZULU_HOST', '192.168.1.250'))
    ap.add_argument('--id', type=int, default=0)
    a = ap.parse_args()
    z = ZuluLoader(a.host)
    if a.cmd == 'ping':
        print('reply in %.0f ms' % (z.ping() * 1000))
    elif a.cmd == 'info':
        print(z.info(a.id))
    elif a.cmd == 'stat':
        print(z.stat(a.id))
    elif a.cmd == 'read':
        sys.stdout.buffer.write(z.read(a.id, int(a.args[0], 0), int(a.args[1], 0)))
    elif a.cmd == 'patch':
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import s950wifi as ak
        writes = ak.read_patch(a.args[0])
        total = sum(len(d) for _, d in writes)
        t = time.time()
        z.write_ranges(a.id, writes, progress=lambda n: sys.stderr.write('\r%d/%d bytes' % (n, total)))
        sys.stderr.write('\n')
        print('applied %d ranges, %d bytes, verified, %.1f s' % (len(writes), total, time.time() - t))


if __name__ == '__main__':
    main()
