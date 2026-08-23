#!/usr/bin/env python3
"""Recover a guest's printk ring buffer from a running QEMU, over QMP.

A kernel booted on a machine that is not its own has no console: its UART is at
an address this board does not have, so every byte it prints goes to whatever
absorbs unassigned accesses. The log still exists, though -- `__log_buf` is
ordinary RAM -- so it can be read out from the outside with no cooperation from
the guest, no console, no serial device and no working driver.

In practice this is the highest-value bring-up sensor there is: it names the
failing subsystem in plain English at the moment a boot-progress ladder can
only say "stopped climbing".

`__log_buf` comes from the image itself; see kimage.py.

    klog.py --qmp /tmp/q.sock --log-buf 0x80e154b0 --base 0x80000000
"""
import argparse
import json
import os
import socket
import struct
import tempfile

# struct printk_log, Linux 3.5 .. 5.9
REC = "QHHHBB"      # ts_nsec, len, text_len, dict_len, facility, flags/level
REC_SZ = 16


def qmp(sock_path):
    s = socket.socket(socket.AF_UNIX)
    s.connect(sock_path)
    s.settimeout(30)
    f = s.makefile("rwb")
    f.readline()                                    # greeting

    def cmd(obj):
        f.write((json.dumps(obj) + "\n").encode())
        f.flush()
        while True:
            line = f.readline()
            if not line:
                return None
            r = json.loads(line)
            if "event" not in r:                    # skip async events
                return r
    cmd({"execute": "qmp_capabilities"})
    return cmd


def read_ring(cmd, paddr, size):
    # pmemsave writes host-side, so a temp file is the transport.
    fd, path = tempfile.mkstemp(prefix="klog-")
    os.close(fd)
    os.unlink(path)
    r = cmd({"execute": "human-monitor-command",
             "arguments": {"command-line":
                           'pmemsave 0x%x 0x%x "%s"' % (paddr, size, path)}})
    if r.get("return"):
        raise RuntimeError("pmemsave: %s" % r["return"].strip())
    with open(path, "rb") as f:
        d = f.read()
    os.unlink(path)
    return d


def decode(d, endian=">"):
    out, off = [], 0
    while off + REC_SZ <= len(d):
        ts, ln, tl, dl, _fac, _fl = struct.unpack_from(endian + REC, d, off)
        # A zero length means the ring wrapped; a wild one means we are not
        # looking at records at all.
        if ln < REC_SZ or ln > 1024 or tl > ln - REC_SZ:
            break
        out.append((ts / 1e9,
                    d[off + REC_SZ:off + REC_SZ + tl].decode("ascii", "replace")))
        off += ln
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qmp", required=True, help="QEMU QMP unix socket")
    ap.add_argument("--log-buf", required=True,
                    help="virtual address of __log_buf (see kimage.py)")
    ap.add_argument("--base", default="0x80000000",
                    help="load base, subtracted to get a physical address")
    ap.add_argument("--size", default="0x10000", help="log_buf_len")
    ap.add_argument("--little", action="store_true")
    args = ap.parse_args()

    paddr = int(args.log_buf, 0) - int(args.base, 0)
    cmd = qmp(args.qmp)
    d = read_ring(cmd, paddr, int(args.size, 0))
    for ts, text in decode(d, "<" if args.little else ">"):
        print("[%11.6f] %s" % (ts, text))


if __name__ == "__main__":
    main()
