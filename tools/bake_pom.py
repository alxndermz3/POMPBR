"""Bakes .pom files offline, from a map's native textures (needs numpy; pillow only for --preview).

The tool inside the game does this for any surface you click (src/Bake.cpp); this is the same thing in numpy, for textures
you want baked without the game, and it's what tests/pom_test.cpp --bake compares the plugin's baker with.

    bake_pom.py <folders of OAT-dumped .dds images> <out folder> [--preview <folder>] [--only <normal image> ...]

OAT: Unlinker --image-format DDS --include-assets image <zone.ff>

The world shaders do  N = normalize(n + x * tangent + y * binormal),  with x and y the two channels of the normal map
mapped to +-2.0157: the map holds surface slopes. Integrating those gives the height that map was made from, in texels,
so the parallax matches the shading. The height goes in the blue channel of a copy of the normal map, which the BC5
original doesn't have, and the patched shader reads it there.
"""
import argparse
import struct
from pathlib import Path

import numpy as np

SLOPE_SCALE = 4.015748
SLOPE_BIAS = -2.015748

# The native textures this test covers.
#   color:   the material's color map. Its dark seams are where the normal map understates how deep a gap goes.
#   depth:   how deep the texture goes, in UV units. The heights measured from the normal maps are printed for comparison:
#            they run shallow, since a normal map only has to shade right, and the wall's is almost flat.
#   gap:     how much of the depth the seams drop on top of the stone's shape.
#   blur:    seam softness, in texels of a 256 texture.
#   flatten: anything broader than this in the normal map, in texels of that texture size, is drift from integrating.
#   radius:  how wide a dark line can be and still count as a seam, in texels of a 256 texture. Default 6.
#   plain:   the surface is level (boards, polished wood) and its normal map is too flat to integrate: what's left of it
#            is compression noise. Height then comes from the seams alone.
#   grain:   how much of the depth the dark grain of the color map drops, on a plain surface.
#   tops, bottoms: the percentiles of the integrated height that count as the surface and as the deepest point. Default
#            92 and 3: stones are mostly top. An atlas is flat with details carved in, and its flat parts must end up at
#            exactly 1, where a ray stops at once: anything below that slides as the camera moves.
#   soften:  how much the finished height is blurred, in texels of a 256 texture. Default 0.35. Sharp height on a low
#            resolution map smears the color down steep faces.
BARK = {"depth": 0.016, "gap": 0.25, "blur": 0.6, "flatten": 60, "radius": 3, "soften": 0.9}

TARGETS = {
    "rock_cobble_stone_pak_n": {"color": "~~-grock_cobble_stone_pak_c-r~1934443d", "depth": 0.017, "gap": 0.75, "blur": 0.7, "flatten": 26},
    "eb_art_floor_brickpath_dry_n": {"color": "~~-geb_art_floor_brickpath_dr~d4ba7479", "depth": 0.015, "gap": 0.8, "blur": 0.8, "flatten": 40},
    "stone_wall_blocks_metal_accent_n": {"color": "~-gstone_wall_blocks_metal_accent_c", "depth": 0.022, "gap": 0.8, "blur": 0.8, "flatten": 40},
    "t6_wood_panel_n": {"color": "~~-gt6_wood_panel_c-rgb&~-rt6~f2d3e628", "depth": 0.03, "gap": 0.7, "blur": 0.4, "flatten": 60, "radius": 2},
    "ma_tile_wood_01_nml": {"color": "~~-gma_tile_wood_01_col-rgb&~~c3478d68", "depth": 0.02, "gap": 1.0, "blur": 0.35, "flatten": 40, "radius": 1.5,
                            "plain": True, "grain": 0.12},
    "wood_panel_off_white_n": {"color": "~~-gwood_panel_off_white_c-rg~23f1d40b", "depth": 0.02, "gap": 1.0, "blur": 0.4, "flatten": 60, "radius": 2,
                               "plain": True, "grain": 0.0},
    "us_art_wall_woodplank_clean_n": {"color": "~~-gus_art_wall_woodplank_cle~1b722b33", "depth": 0.025, "gap": 1.0, "blur": 0.4, "flatten": 60, "radius": 2,
                                      "plain": True, "grain": 0.1},

    "tree_oak_trunk_nml": {"color": "~-gtree_oak_trunk_col", **BARK},
    "tree_birch_trunk_nml": {"color": "~-gtree_birch_trunk_col", **BARK},
    "p6_tree_acacia_bark_n": {"color": "~-gp6_tree_acacia_bark_c", **BARK},
    "p6_tree_acacia_thorn_bark_n": {"color": "~-gp6_tree_acacia_thorn_bark_c", **BARK},
    "p6_tree_palm_date_bark_n": {"color": "~-gp6_tree_palm_date_bark_c", **BARK},
    "p6_tree_palm_date_base_n": {"color": "~-gp6_tree_palm_date_base_c", **BARK},
    "p6_tree_palm_date_cuts_n": {"color": "~-gp6_tree_palm_date_cuts_c", **BARK},
    "p6_tree_palm_fan_cali_bark_n": {"color": "~~-gp6_tree_palm_fan_cali_bar~77235f49", **BARK},
    "t5_foliage_tree_banana_trunk01_n": {"color": "~-gt5_foliage_tree_banana_trunk01_c", **BARK},
    "t5_foliage_tree_sorbus_bark01_n": {"color": "~-gt5_foliage_tree_aqui_bark01a_c", **BARK},
    "t5_foliage_tree_palm_coco_bark_comb_n": {"color": "~-gt5_foliage_tree_palm_coco_~da00c081", **BARK},
    "t5_foliage_tree_palm_fan_bark01_n": {"color": "~-gt5_foliage_tree_palm_fan_bark01_c", **BARK},
    "t5_foliage_tree_palm_fan_bark03_n": {"color": "~-gt5_foliage_tree_palm_fan_bark03_c", **BARK},
}


def read_dds(path):
    """Returns (width, height, mip count, format, raw data, offset of the first mip)."""
    data = path.read_bytes()
    if data[:4] != b"DDS ":
        raise ValueError(f"{path.name} isn't a DDS file")
    height, width = struct.unpack_from("<II", data, 12)
    mips = max(struct.unpack_from("<I", data, 28)[0], 1)
    fourcc = data[84:88]
    offset = 128
    if fourcc == b"DX10":
        fourcc = struct.unpack_from("<I", data, 128)[0]
        offset = 148
    return width, height, mips, fourcc, data, offset


def bc4_tables(blocks):
    """blocks: (n, 8) uint8. Returns (n, 16) decoded values, for BC4/BC5 channels and BC3 alpha."""
    r0 = blocks[:, 0].astype(np.int32)
    r1 = blocks[:, 1].astype(np.int32)
    table = np.zeros((len(blocks), 8), np.int32)
    table[:, 0] = r0
    table[:, 1] = r1
    eight = r0 > r1
    for i in range(1, 7):
        table[:, i + 1] = np.where(eight, ((7 - i) * r0 + i * r1 + 3) // 7, 0)
    for i in range(1, 5):
        table[:, i + 1] = np.where(eight, table[:, i + 1], ((5 - i) * r0 + i * r1 + 2) // 5)
    table[:, 6] = np.where(eight, table[:, 6], 0)
    table[:, 7] = np.where(eight, table[:, 7], 255)
    bits = np.zeros(len(blocks), np.uint64)
    for i in range(6):
        bits |= blocks[:, 2 + i].astype(np.uint64) << np.uint64(8 * i)
    selectors = np.stack([(bits >> np.uint64(3 * i)) & np.uint64(7) for i in range(16)], axis=1).astype(np.int32)
    return np.take_along_axis(table, selectors, axis=1).astype(np.uint8)


def bc1_colors(blocks):
    """blocks: (n, 8) uint8. Returns (n, 16, 3) decoded colors, always in four-color mode as BC3 does."""
    c0 = blocks[:, 0].astype(np.int32) | (blocks[:, 1].astype(np.int32) << 8)
    c1 = blocks[:, 2].astype(np.int32) | (blocks[:, 3].astype(np.int32) << 8)

    def expand(c):
        return np.stack([(c >> 11 & 31) * 255 // 31, (c >> 5 & 63) * 255 // 63, (c & 31) * 255 // 31], axis=1)

    a, b = expand(c0), expand(c1)
    table = np.stack([a, b, (2 * a + b) // 3, (a + 2 * b) // 3], axis=1)
    bits = blocks[:, 4].astype(np.uint32) | blocks[:, 5].astype(np.uint32) << 8 | blocks[:, 6].astype(np.uint32) << 16 | blocks[:, 7].astype(np.uint32) << 24
    selectors = np.stack([(bits >> (2 * i)) & 3 for i in range(16)], axis=1).astype(np.int32)
    return np.take_along_axis(table, selectors[:, :, None].repeat(3, axis=2), axis=1).astype(np.uint8)


def untile(values, blocks_wide, blocks_high):
    """(n, 16, ...) block values to an image."""
    shape = values.shape[2:]
    grid = values.reshape(blocks_high, blocks_wide, 4, 4, *shape)
    return np.concatenate([np.concatenate(list(row), axis=1) for row in grid], axis=0)


def decode_level(data, offset, width, height, fourcc):
    """Returns (image with channels last, bytes used)."""
    wide, high = max((width + 3) // 4, 1), max((height + 3) // 4, 1)
    count = wide * high
    if fourcc in (b"ATI2", b"BC5U", 83):
        raw = np.frombuffer(data, np.uint8, count * 16, offset).reshape(count, 16)
        r = untile(bc4_tables(raw[:, :8]), wide, high)
        g = untile(bc4_tables(raw[:, 8:]), wide, high)
        return np.dstack([r, g])[:height, :width], count * 16
    if fourcc in (b"DXT1", 71):
        raw = np.frombuffer(data, np.uint8, count * 8, offset).reshape(count, 8)
        return untile(bc1_colors(raw), wide, high)[:height, :width], count * 8
    if fourcc in (b"DXT5", 77):
        raw = np.frombuffer(data, np.uint8, count * 16, offset).reshape(count, 16)
        return untile(bc1_colors(raw[:, 8:]), wide, high)[:height, :width], count * 16
    raise ValueError(f"unsupported format {fourcc!r}")


def decode_mips(path):
    width, height, mips, fourcc, data, offset = read_dds(path)
    levels = []
    for _ in range(mips):
        image, used = decode_level(data, offset, width, height, fourcc)
        levels.append(image)
        offset += used
        width, height = max(width // 2, 1), max(height // 2, 1)
    return levels


def frequencies(height, width):
    return np.fft.fftfreq(height)[:, None], np.fft.rfftfreq(width)[None, :]


def gaussian(image, sigma):
    """Blur that wraps around the edges, as the texture does."""
    fy, fx = frequencies(*image.shape)
    return np.fft.irfft2(np.fft.rfft2(image) * np.exp(-2 * (np.pi * sigma) ** 2 * (fx * fx + fy * fy)), image.shape)


def integrate(dx, dy):
    """Least-squares height for a gradient field (Frankot-Chellappa). Periodic, so what tiles stays tiling."""
    fy, fx = frequencies(*dx.shape)
    wx, wy = 2j * np.pi * fx, 2j * np.pi * fy
    power = np.abs(wx) ** 2 + np.abs(wy) ** 2
    power[0, 0] = 1
    result = (np.conj(wx) * np.fft.rfft2(dx) + np.conj(wy) * np.fft.rfft2(dy)) / power
    result[0, 0] = 0
    return np.fft.irfft2(result, dx.shape)


def curl(dx, dy):
    """How far a gradient field is from being one: 0 for a real height."""
    return np.abs((np.roll(dx, -1, 0) - dx) - (np.roll(dy, -1, 1) - dy)).mean()


def smoothstep(low, high, x):
    t = np.clip((x - low) / (high - low), 0, 1)
    return t * t * (3 - 2 * t)


def bake_height(normal, color, target):
    """Returns (height 0..1 with 1 = the surface itself, a line of diagnostics)."""
    rg = normal.astype(np.float64) / 255
    sx, sy = rg[..., 0] * SLOPE_SCALE + SLOPE_BIAS, rg[..., 1] * SLOPE_SCALE + SLOPE_BIAS
    sx, sy = sx - sx.mean(), sy - sy.mean()

    # A bump's normal leans away from it, so slope = -gradient. Green is flipped if the map's y runs against v: the
    # wrong sign isn't a gradient of anything, which is what the curl measures.
    curl_plain, curl_flipped = curl(-sx, -sy), curl(-sx, sy)
    flip_green = curl_flipped < curl_plain
    raw = integrate(-sx, sy if flip_green else -sy)

    luma = (color[..., :3].astype(np.float64) * [0.299, 0.587, 0.114]).sum(axis=2) / 255
    width = raw.shape[1]
    scale = width / 256
    flat = raw - gaussian(raw, target["flatten"] * 0.5)

    # Stones: rounded shape from the normal map, measured down from their tops, so a stone's top sits at the surface.
    tops = np.percentile(flat, target.get("tops", 92))
    bottoms = np.percentile(flat, target.get("bottoms", 3))
    shape = np.clip((flat - bottoms) / (tops - bottoms), 0, 1)
    depth_texels = tops - bottoms

    # Seams: dark against their surroundings in the color map, and low in the normal map.
    local = gaussian(luma, target.get("radius", 6) * scale)
    darkness = local - luma
    seam = smoothstep(0.35, 1.0, darkness / max(np.percentile(darkness, 97), 1e-3))
    if target.get("plain"):
        # A board's surface stays at the top; only its grain and the gaps between boards go down from there.
        grain = np.clip(darkness / max(np.percentile(np.abs(darkness), 99), 1e-3), 0, 1)
        height = (1 - target["grain"] * gaussian(grain, 0.4 * scale)) * (1 - target["gap"] * gaussian(seam, target["blur"] * scale))
    else:
        seam = gaussian(seam, target["blur"] * scale) * smoothstep(0.75, 0.35, shape)
        height = np.clip(shape * (1 - target["gap"] * 0.5) + (target["gap"] * 0.5), 0, 1) * (1 - target["gap"] * seam)
    height = gaussian(height, target.get("soften", 0.35) * scale)
    height = np.clip((height - height.min()) / (height.max() - height.min()), 0, 1)

    agreement = np.corrcoef(flat.ravel(), luma.ravel())[0, 1]
    line = (f"curl {curl_plain:.4f} as is, {curl_flipped:.4f} green flipped -> {'flipped' if flip_green else 'as is'}; "
            f"height vs color correlation {agreement:+.2f}; normal map's own depth {depth_texels / width:.4f}, baked {target['depth']}")
    return height, line


def box_down(image):
    h, w = image.shape
    out_h, out_w = max(h // 2, 1), max(w // 2, 1)
    if h == 1 or w == 1:
        return image[:out_h, :out_w].copy()
    return image[:out_h * 2, :out_w * 2].reshape(out_h, 2, out_w, 2).mean(axis=(1, 3))


def find_image(folders, name):
    for folder in folders:
        path = folder / f"{name}.dds"
        if path.exists():
            return path
    raise FileNotFoundError(f"{name}.dds isn't in any of the image folders")


def write_pom(path, normal_levels, height, depth_uv):
    """Layout: src/PomFile.h."""
    with open(path, "wb") as out:
        width, high = normal_levels[0].shape[1], normal_levels[0].shape[0]
        out.write(struct.pack("<4sIIIf", b"POM1", width, high, len(normal_levels), depth_uv))
        level_height = height
        for level in normal_levels:
            blue = np.rint(np.clip(level_height, 0, 1) * 255).astype(np.uint8)
            alpha = np.full(blue.shape, 255, np.uint8)
            out.write(np.dstack([level[..., 0], level[..., 1], blue, alpha]).tobytes())
            level_height = box_down(level_height)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("images", type=Path, nargs="+")
    parser.add_argument("out", type=Path)
    parser.add_argument("--preview", type=Path)
    parser.add_argument("--only", nargs="+")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    for name, target in TARGETS.items():
        if args.only and name not in args.only:
            continue
        normal_levels = decode_mips(find_image(args.images, name))
        color = decode_mips(find_image(args.images, target["color"]))[0]
        height, line = bake_height(normal_levels[0], color, target)
        write_pom(args.out / f"{name}.pom", normal_levels, height, float(target["depth"]))
        print(f"{name}: {line}")
        if args.preview:
            from PIL import Image

            args.preview.mkdir(parents=True, exist_ok=True)
            Image.fromarray(np.rint(height * 255).astype(np.uint8)).save(args.preview / f"{name}_height.png")


if __name__ == "__main__":
    main()
