import argparse
import struct
import sys
import zlib

SIGNATURE = b"\x89PNG\r\n\x1a\n"
CHANNELS = {0: 1, 2: 3, 4: 2, 6: 4}


class PngError(Exception):
    pass


def chunks(data):
    if not data.startswith(SIGNATURE):
        raise PngError("not a PNG")
    pos = len(SIGNATURE)
    while pos < len(data):
        if pos + 8 > len(data):
            raise PngError("truncated chunk header")
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        crc = data[pos + 8 + length:pos + 12 + length]
        if len(body) != length or len(crc) != 4:
            raise PngError("truncated chunk")
        if zlib.crc32(kind + body) != struct.unpack(">I", crc)[0]:
            raise PngError("bad CRC in %s" % kind.decode("latin-1"))
        yield kind, body
        if kind == b"IEND":
            return
        pos += 12 + length
    raise PngError("no IEND")


def unfilter(raw, width, height, bpp):
    stride = width * bpp
    if len(raw) != height * (stride + 1):
        raise PngError("image data is %d bytes, want %d" % (len(raw), height * (stride + 1)))
    out = bytearray(height * stride)
    prev = bytearray(stride)
    for y in range(height):
        start = y * (stride + 1)
        ftype = raw[start]
        line = bytearray(raw[start + 1:start + 1 + stride])
        if ftype == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif ftype == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ftype == 3:
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif ftype == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if pa <= pb and pa <= pc else b if pb <= pc else c
                line[i] = (line[i] + pred) & 0xFF
        elif ftype != 0:
            raise PngError("bad filter type %d in row %d" % (ftype, y))
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return out


def to_grey(pixels, width, height, ctype):
    if ctype == 0:
        return bytes(pixels)
    n = CHANNELS[ctype]
    out = bytearray(width * height)
    for i in range(width * height):
        px = pixels[i * n:(i + 1) * n]
        g = px[0] if ctype == 4 else (299 * px[0] + 587 * px[1] + 114 * px[2] + 500) // 1000
        if ctype in (4, 6):
            a = px[-1]
            g = (g * a + 255 * (255 - a) + 127) // 255
        out[i] = g
    return bytes(out)


def convert(data, width, height):
    header = None
    idat = []
    for kind, body in chunks(data):
        if kind == b"IHDR":
            if len(body) != 13:
                raise PngError("bad IHDR")
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat.append(body)
    if header is None:
        raise PngError("no IHDR")
    w, h, depth, ctype, compression, filtering, interlace = header
    if (w, h) != (width, height):
        raise PngError("image is %dx%d, want %dx%d" % (w, h, width, height))
    if depth != 8 or ctype not in CHANNELS:
        raise PngError("only 8-bit grey, grey+alpha, RGB and RGBA are supported (depth %d, colour type %d)"
                       % (depth, ctype))
    if compression != 0 or filtering != 0:
        raise PngError("unknown compression or filter method")
    if interlace != 0:
        raise PngError("interlaced PNGs are not supported")
    try:
        raw = zlib.decompress(b"".join(idat))
    except zlib.error as e:
        raise PngError("bad image data: %s" % e) from e
    return to_grey(unfilter(raw, w, h, CHANNELS[ctype]), w, h, ctype)


def parse_size(text):
    w, sep, h = text.partition("x")
    if not sep or not w.isdigit() or not h.isdigit() or int(w) == 0 or int(h) == 0:
        raise argparse.ArgumentTypeError("size must be WxH, such as 1264x1680")
    return int(w), int(h)


def main(argv=None):
    ap = argparse.ArgumentParser(description="Convert an 8-bit PNG to raw 8-bit grey for the onyx-epd sleep image")
    ap.add_argument("--size", type=parse_size, required=True)
    ap.add_argument("input")
    ap.add_argument("output")
    args = ap.parse_args(argv)
    with open(args.input, "rb") as f:
        data = f.read()
    try:
        grey = convert(data, *args.size)
    except PngError as e:
        print("png2grey: %s: %s" % (args.input, e), file=sys.stderr)
        return 1
    with open(args.output, "wb") as f:
        f.write(grey)
    return 0


if __name__ == "__main__":
    sys.exit(main())
