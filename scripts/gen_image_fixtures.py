"""Builds the encoded-image fixtures the resource tests compile in.

Two outputs, because the suites that use them have different build conditions:

  tests/resource/image_fixtures.inc      -- PNG and Radiance HDR, decoded by stb. Only compiled into
                                            a CATALYST_RESOURCE_STB=ON build.
  tests/resource/container_fixtures.inc  -- KTX2 and DDS, read by this module's own code, which is
                                            in every build.

Everything here is assembled byte by byte rather than written by an encoder, so a test can assert on
the texel or the block it expects and a failure names a field rather than "the decode changed".
Level payloads in the container fixtures are runs of a single distinctive byte -- 0x10 for level 0
of layer 0, 0x11 for level 0 of layer 1, and so on -- which is what makes the KTX2 transposition
checkable by reading four bytes.

Run from the repository root:  python scripts/gen_image_fixtures.py
"""

import io, struct, zlib

# =============================================================================
# Source formats: PNG and Radiance HDR
# =============================================================================

def png(width, height, colortype, raw_rows, corrupt_idat=False):
    def chunk(tag, data):
        c = tag + data
        return struct.pack('>I', len(data)) + c + struct.pack('>I', zlib.crc32(c) & 0xffffffff)
    ihdr = struct.pack('>IIBBBBB', width, height, 8, colortype, 0, 0, 0)
    raw = b''.join(b'\x00' + row for row in raw_rows)
    idat = zlib.compress(raw, 9)
    if corrupt_idat:
        idat = idat[:2] + bytes((idat[2] ^ 0xFF,)) + b'\x13\x37' + idat[3:]
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr) + chunk(b'IDAT', idat) + chunk(b'IEND', b''))

def hdr(width, height, rgbe_rows):
    header = b'#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n'
    header += ('-Y %d +X %d\n' % (height, width)).encode('ascii')
    return header + b''.join(b''.join(bytes(px) for px in row) for row in rgbe_rows)

source_images = {}
source_images['png_rgba_2x2'] = png(2, 2, 6, [
    bytes([255, 0, 0, 255,  0, 255, 0, 255]),
    bytes([0, 0, 255, 255,  255, 255, 255, 128]),
])
source_images['png_rgb_2x2'] = png(2, 2, 2, [
    bytes([255, 0, 0,  0, 255, 0]),
    bytes([0, 0, 255,  10, 20, 30]),
])
source_images['png_gray_2x2'] = png(2, 2, 0, [bytes([0, 64]), bytes([128, 255])])
source_images['png_gray_alpha_2x2'] = png(2, 2, 4, [
    bytes([10, 255,  20, 128]),
    bytes([30, 64,   40, 0]),
])
source_images['png_corrupt_2x2'] = png(2, 2, 6, [
    bytes([255, 0, 0, 255,  0, 255, 0, 255]),
    bytes([0, 0, 255, 255,  255, 255, 255, 128]),
], corrupt_idat=True)
source_images['hdr_2x2'] = hdr(2, 2, [
    [(128, 64, 32, 129), (128, 128, 128, 129)],
    [(64, 64, 64, 129),  (0, 0, 0, 0)],
])

# A 4x4 sRGB PNG whose texels are the two extremes of the gamma curve, so that a mip generated from
# it lands on a value that distinguishes sRGB-correct averaging (188) from averaging the raw bytes
# (128). Columns alternate, so every 2x2 neighbourhood is half black and half white.
source_images['png_srgb_4x4'] = png(4, 4, 6, [
    bytes([0, 0, 0, 255,  255, 255, 255, 255,  0, 0, 0, 255,  255, 255, 255, 255]),
] * 4)

# =============================================================================
# Shared level-payload helper
# =============================================================================

def payload(level, layer, size):
    """A run of one distinctive byte, identifying the level and layer it belongs to."""
    return bytes([0x10 + level * 0x10 + layer]) * size


def block_size(fmt_block_bytes, width, height):
    """Tightly packed bytes of one 4x4-block surface."""
    return ((width + 3) // 4) * ((height + 3) // 4) * fmt_block_bytes


# =============================================================================
# KTX2
# =============================================================================

KTX2_ID = bytes([0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A])


def ktx2(vk_format, width, height, level_sizes, layers=0, faces=1, depth=0,
         supercompression=0, level_count=None, smallest_first=True, level_size_override=None):
    """Assembles an uncompressed KTX2.

    `level_sizes` is the byte length of ONE image at each level; the stored level is that times the
    number of layers times the number of faces. `smallest_first` lays the data out the way real
    writers do -- last level first -- which is exactly why the reader must use the level index and
    not assume an order.
    """
    stored_layers = max(layers, 1)
    total_layers = stored_layers * faces
    levels = len(level_sizes)
    declared_levels = levels if level_count is None else level_count

    header = struct.pack('<9I', vk_format, 1, width, height, depth,
                         layers, faces, declared_levels, supercompression)
    index = struct.pack('<4I2Q', 0, 0, 0, 0, 0, 0)

    data_start = len(KTX2_ID) + len(header) + len(index) + levels * 24

    # Each level's payload is every layer of that level, back to back.
    level_data = []
    for level, one_image in enumerate(level_sizes):
        level_data.append(b''.join(payload(level, layer, one_image) for layer in range(total_layers)))

    order = list(reversed(range(levels))) if smallest_first else list(range(levels))

    offsets = {}
    cursor = data_start
    for level in order:
        offsets[level] = cursor
        cursor += len(level_data[level])

    level_index = b''
    for level in range(levels):
        declared = len(level_data[level])
        if level_size_override is not None and level_size_override[0] == level:
            declared = level_size_override[1]
        level_index += struct.pack('<3Q', offsets[level], declared, declared)

    body = bytearray(cursor - data_start)
    for level in order:
        start = offsets[level] - data_start
        body[start:start + len(level_data[level])] = level_data[level]

    return KTX2_ID + header + index + level_index + bytes(body)


container_images = {}

# RGBA8 4x4 with a complete three-level chain, one layer. The plain case.
container_images['ktx2_rgba8_4x4'] = ktx2(37, 4, 4, [4 * 4 * 4, 2 * 2 * 4, 1 * 1 * 4])

# BC7 8x8, two levels, two array layers. This is the transposition test: the file holds both layers
# of level 0 followed by both layers of level 1, and the image must hold layer 0's whole chain then
# layer 1's.
container_images['ktx2_bc7_8x8_array'] = ktx2(145, 8, 8, [block_size(16, 8, 8), block_size(16, 4, 4)], layers=2)

# BC1 4x4 cube map: six faces become six array layers.
container_images['ktx2_bc1_cube'] = ktx2(133, 4, 4, [block_size(8, 4, 4)], faces=6)

# levelCount == 0: the file stores one level and asks the application to make the rest.
container_images['ktx2_no_levels'] = ktx2(37, 4, 4, [4 * 4 * 4], level_count=0)

# Zstandard supercompression, which this build has no decompressor for.
container_images['ktx2_zstd'] = ktx2(37, 4, 4, [4 * 4 * 4], supercompression=2)

# VK_FORMAT_ASTC_4x4_UNORM_BLOCK (157): a legitimate file with no rendering::format counterpart.
container_images['ktx2_astc'] = ktx2(157, 4, 4, [4 * 4 * 4])

# A level index that disagrees with the format's own arithmetic by four bytes.
container_images['ktx2_bad_level_size'] = ktx2(37, 4, 4, [4 * 4 * 4, 2 * 2 * 4],
                                               level_size_override=(1, 2 * 2 * 4 - 4))

# =============================================================================
# DDS
# =============================================================================

DDSD_CAPS, DDSD_HEIGHT, DDSD_WIDTH, DDSD_PITCH = 0x1, 0x2, 0x4, 0x8
DDSD_PIXELFORMAT, DDSD_MIPMAPCOUNT, DDSD_LINEARSIZE, DDSD_DEPTH = 0x1000, 0x20000, 0x80000, 0x800000

DDPF_ALPHAPIXELS, DDPF_ALPHA, DDPF_FOURCC, DDPF_RGB, DDPF_LUMINANCE = 0x1, 0x2, 0x4, 0x40, 0x20000

DDSCAPS_COMPLEX, DDSCAPS_TEXTURE, DDSCAPS_MIPMAP = 0x8, 0x1000, 0x400000
DDSCAPS2_CUBEMAP, DDSCAPS2_ALL_FACES, DDSCAPS2_VOLUME = 0x200, 0xFC00, 0x200000


def fourcc(text):
    return struct.unpack('<I', text.encode('ascii'))[0]


def dds(width, height, pixelformat, data, mips=1, depth=0, caps2=0, dxt10=None, truncate=0):
    """Assembles a DDS. `pixelformat` is the 32-byte DDS_PIXELFORMAT as a tuple of its eight fields."""
    flags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT
    if mips > 1:
        flags |= DDSD_MIPMAPCOUNT
    if depth > 0:
        flags |= DDSD_DEPTH

    caps = DDSCAPS_TEXTURE
    if mips > 1 or caps2 != 0:
        caps |= DDSCAPS_COMPLEX
    if mips > 1:
        caps |= DDSCAPS_MIPMAP

    header = struct.pack('<7I', 124, flags, height, width, 0, depth, mips)
    header += b'\x00' * 44                       # dwReserved1[11]
    header += struct.pack('<8I', *pixelformat)   # DDS_PIXELFORMAT
    header += struct.pack('<5I', caps, caps2, 0, 0, 0)
    assert len(header) == 124, len(header)

    out = b'DDS ' + header
    if dxt10 is not None:
        out += struct.pack('<5I', *dxt10)
    out += data
    return out[:len(out) - truncate] if truncate else out


def pf_rgb(bit_count, r, g, b, a):
    flags = DDPF_RGB | (DDPF_ALPHAPIXELS if a else 0)
    return (32, flags, 0, bit_count, r, g, b, a)


def pf_fourcc(code):
    return (32, DDPF_FOURCC, code if isinstance(code, int) else fourcc(code), 0, 0, 0, 0, 0)


# BGRA8 2x2 with a 1x1 mip. Real texels, so the test can name the colours it expects -- and in
# memory order, which for BGRA means blue first.
bgra_level0 = bytes([
    0, 0, 255, 255,      # red
    0, 255, 0, 255,      # green
    255, 0, 0, 255,      # blue
    255, 255, 255, 128,  # white, half alpha
])
bgra_level1 = bytes([64, 65, 66, 67])
container_images['dds_bgra8_2x2'] = dds(
    2, 2, pf_rgb(32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000),
    bgra_level0 + bgra_level1, mips=2)

# Legacy DXT1, one level.
container_images['dds_dxt1_4x4'] = dds(4, 4, pf_fourcc('DXT1'), payload(0, 0, block_size(8, 4, 4)))

# BC7 through the DX10 header, four levels: 8x8, 4x4, 2x2, 1x1. Every level below the first is a
# single block whatever its extent, which is the arithmetic `format_image_size_bytes` exists for.
bc7_levels = [block_size(16, 8, 8), block_size(16, 4, 4), block_size(16, 2, 2), block_size(16, 1, 1)]
container_images['dds_bc7_8x8'] = dds(
    8, 8, pf_fourcc('DX10'),
    b''.join(payload(level, 0, size) for level, size in enumerate(bc7_levels)),
    mips=4, dxt10=(98, 3, 0, 1, 0))

# A legacy cube map: six faces, each a whole one-level chain. Faces become array layers.
container_images['dds_dxt1_cube'] = dds(
    4, 4, pf_fourcc('DXT1'),
    b''.join(payload(0, face, block_size(8, 4, 4)) for face in range(6)),
    caps2=DDSCAPS2_CUBEMAP | DDSCAPS2_ALL_FACES)

# A partial cube map -- four of the six faces, which DDS permits and reflection probes used.
container_images['dds_dxt1_cube_partial'] = dds(
    4, 4, pf_fourcc('DXT1'),
    b''.join(payload(0, face, block_size(8, 4, 4)) for face in range(4)),
    caps2=DDSCAPS2_CUBEMAP | 0x400 | 0x800 | 0x1000 | 0x2000)

# 32-bit RGB with no alpha channel: refused, with an explanation rather than a transparent texture.
container_images['dds_x8r8g8b8'] = dds(
    2, 2, pf_rgb(32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0), b'\x00' * 16)

# A 24-bit layout, which rendering::format has no spelling of at all.
container_images['dds_rgb24'] = dds(
    2, 2, pf_rgb(24, 0x00FF0000, 0x0000FF00, 0x000000FF, 0), b'\x00' * 12)

# The declared four levels are there but the last block is missing.
container_images['dds_bc7_truncated'] = dds(
    8, 8, pf_fourcc('DX10'),
    b''.join(payload(level, 0, size) for level, size in enumerate(bc7_levels)),
    mips=4, dxt10=(98, 3, 0, 1, 0), truncate=1)

# More mip levels than an 8x8 image can have.
container_images['dds_too_many_mips'] = dds(
    8, 8, pf_fourcc('DX10'), b'\x00' * 4096, mips=9, dxt10=(98, 3, 0, 1, 0))

# An array size that overflows when multiplied by six in 32 bits: 0x80000000 * 6 wraps to zero, and
# a reader that did the multiplication in the file's own width would then size the whole image at
# nothing and hand back an `image` claiming no layers. Built by patching a good file's DX10 header,
# so the only thing wrong with it is the number under test.
_dds_overflow = bytearray(container_images['dds_bc7_8x8'])
struct.pack_into('<I', _dds_overflow, 4 + 124 + 8, 0x00000004)   # miscFlag: DDS_RESOURCE_MISC_TEXTURECUBE
struct.pack_into('<I', _dds_overflow, 4 + 124 + 12, 0x80000000)  # arraySize
container_images['dds_absurd_array_size'] = bytes(_dds_overflow)

# The same shape in KTX2: layerCount * faceCount overflows 32 bits, and the product is far more
# layers than a 236-byte file could hold either way.
_ktx2_overflow = bytearray(container_images['ktx2_rgba8_4x4'])
struct.pack_into('<I', _ktx2_overflow, 12 + 5 * 4, 0x40000000)   # layerCount
struct.pack_into('<I', _ktx2_overflow, 12 + 6 * 4, 6)            # faceCount
container_images['ktx2_absurd_layers'] = bytes(_ktx2_overflow)

# A header whose dwSize is not 124: not a DDS whatever its magic number says.
container_images['dds_bad_header_size'] = bytearray(
    dds(4, 4, pf_fourcc('DXT1'), payload(0, 0, block_size(8, 4, 4))))
container_images['dds_bad_header_size'][4] = 120
container_images['dds_bad_header_size'] = bytes(container_images['dds_bad_header_size'])

# =============================================================================
# Emission
# =============================================================================

def carray(name, data):
    out = ['    constexpr std::uint8_t %s[] = {' % name]
    for i in range(0, len(data), 12):
        out.append('        ' + ' '.join('0x%02x,' % b for b in data[i:i+12]))
    out.append('    };')
    return '\n'.join(out)


def emit(path, banner, images):
    body = '\n\n'.join(carray(k, v) for k, v in images.items())
    io.open(path, 'w', encoding='ascii', newline='\n').write(banner + '\n' + body + '\n')
    for k, v in images.items():
        print('%-28s %6d bytes  ->  %s' % (k, len(v), path))


emit('tests/resource/image_fixtures.inc', """// Generated by scripts/gen_image_fixtures.py -- do not edit by hand.
//
// Hand-built encoded images, small enough to reason about texel by texel. They are compiled in
// rather than read from disk so the decoder tests stay independent of the vfs and of the working
// directory a test runner happens to use.
""", source_images)

emit('tests/resource/container_fixtures.inc', """// Generated by scripts/gen_image_fixtures.py -- do not edit by hand.
//
// Hand-built KTX2 and DDS containers. Every level's payload is a run of one byte identifying the
// level and the layer it belongs to -- 0x10 is level 0 layer 0, 0x11 is level 0 layer 1, 0x20 is
// level 1 layer 0 -- so a test can prove the reader put each surface where it belongs by looking at
// a single byte of it.
//
// These are read by this module's own code and not by stb, so unlike image_fixtures.inc they are
// compiled into every build.
""", container_images)
