"""Receives the screen images streamed by test/test_screenshots and saves PNGs.

Usage (run by tools/capture_screenshots.sh, with PlatformIO's Python, which has pyserial):
    python tools/capture_screenshots.py /dev/cu.usbmodem1201 docs/screens
Protocol: board prints READY until it gets 'G'; then per image
    IMG <name> <w> <h>   /   B:<base64 of one RGB565-LE row>  x h   /   END <name>
and DONE at the end. Any other line (logs) is ignored.
"""
import base64, os, struct, sys, time, zlib
import serial

# Areas painted over before saving (x, y, w, h): private data still visible on screen.
REDACT = {
    "settings": [(20, 263, 450, 28)],     # "Conectado: <nome da rede> | IP <...>"
}

def png(path, w, h, rgb):
    def chunk(t, d):
        c = t + d
        return struct.pack(">I", len(d)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))

def to_rgb(rows, w):
    out = bytearray()
    for row in rows:
        for i in range(w):
            v = row[2 * i] | row[2 * i + 1] << 8
            r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
            out += bytes(((r * 527 + 23) >> 6, (g * 259 + 33) >> 6, (b * 527 + 23) >> 6))
    return out

def redact(rgb, w, boxes):
    for x0, y0, bw, bh in boxes:
        for y in range(y0, y0 + bh):
            for x in range(x0, x0 + bw):
                rgb[(y * w + x) * 3:(y * w + x) * 3 + 3] = b"\x30\x30\x30"

def main():
    port, outdir = sys.argv[1], sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    ser = serial.Serial(port, 115200, timeout=1)
    deadline = time.time() + 180
    while time.time() < deadline:                      # handshake
        line = ser.readline().decode(errors="ignore").strip()
        if line == "READY":
            ser.write(b"G"); break
    else:
        sys.exit("placa não respondeu READY (o firmware de captura foi gravado?)")
    name, w, h, rows, saved = None, 0, 0, [], []
    deadline = time.time() + 600
    while time.time() < deadline:
        line = ser.readline().decode(errors="ignore").strip()
        if line.startswith("IMG "):
            _, name, w, h = line.split(); w, h, rows = int(w), int(h), []
        elif line.startswith("B:") and name:
            rows.append(base64.b64decode(line[2:]))
        elif line.startswith("END ") and name:
            if len(rows) != h:
                print(f"  {name}: {len(rows)}/{h} linhas recebidas, imagem descartada")
            else:
                rgb = to_rgb(rows, w)
                redact(rgb, w, REDACT.get(name, []))
                png(os.path.join(outdir, name + ".png"), w, h, bytes(rgb))
                saved.append(name); print(f"  {name}.png")
            name = None
        elif line == "DONE":
            break
    print(f"{len(saved)} imagens salvas em {outdir}")
    sys.exit(0 if saved else 1)

if __name__ == "__main__":
    main()
