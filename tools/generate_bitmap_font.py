"""Generate the embedded 1-bpp Share Tech Mono font used by the e-paper UI."""

from pathlib import Path
from math import ceil

from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "font" / "Share-TechMono.ttf"
OUTPUT = ROOT / "main" / "font_share_tech_mono.h"
FIRST_CHAR = 32
LAST_CHAR = 90
EXTRA_CHARACTERS = "°"
STATUS_FONT_SIZE = 18
FONT_SIZES = (17, 22, 30, 46)


def render_font(size: int):
    font = ImageFont.truetype(str(SOURCE), size)
    characters = [chr(code) for code in range(FIRST_CHAR, LAST_CHAR + 1)] + list(EXTRA_CHARACTERS)
    boxes = [font.getbbox(char) for char in characters if char != " "]
    top = min(box[1] for box in boxes)
    bottom = max(box[3] for box in boxes)
    width = max(ceil(font.getlength(char)) for char in characters)
    width = max(width, max(box[2] for box in boxes))
    height = bottom - top
    stride = (width + 7) // 8
    output = bytearray()

    for char in characters:
        image = Image.new("1", (width, height), 0)
        draw = ImageDraw.Draw(image)
        draw.text((0, -top), char, font=font, fill=1)
        for y in range(height):
            for byte_x in range(stride):
                value = 0
                for bit in range(8):
                    x = byte_x * 8 + bit
                    if x < width and image.getpixel((x, y)):
                        value |= 0x80 >> bit
                output.append(value)
    return width, height, stride, output


def format_bytes(data: bytearray) -> list[str]:
    rows = []
    for offset in range(0, len(data), 16):
        rows.append("    " + ", ".join(f"0x{value:02x}" for value in data[offset:offset + 16]) + ",")
    return rows


def main() -> None:
    lines = [
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        "/* Generated from font/Share-TechMono.ttf by tools/generate_bitmap_font.py. */",
        f"#define SHARE_TECH_MONO_FIRST_CHAR {FIRST_CHAR}",
        f"#define SHARE_TECH_MONO_LAST_CHAR  {LAST_CHAR}",
        f"#define SHARE_TECH_MONO_DEGREE_INDEX {LAST_CHAR - FIRST_CHAR + 1}",
        "",
        "typedef struct {",
        "    uint8_t width;",
        "    uint8_t height;",
        "    uint8_t stride;",
        "    const uint8_t *data;",
        "} bitmap_font_t;",
        "",
    ]

    status_width, status_height, status_stride, status_data = render_font(STATUS_FONT_SIZE)
    lines.append(f"static const uint8_t s_share_tech_mono_status_data[{len(status_data)}] = {{")
    lines.extend(format_bytes(status_data))
    lines.extend([
        "};",
        "",
        "static const bitmap_font_t s_share_tech_mono_status = {",
        f"    .width = {status_width},",
        f"    .height = {status_height},",
        f"    .stride = {status_stride},",
        "    .data = s_share_tech_mono_status_data,",
        "};",
        "",
    ])

    fonts = []
    for index, size in enumerate(FONT_SIZES, start=1):
        width, height, stride, data = render_font(size)
        name = f"s_share_tech_mono_{size}"
        lines.append(f"static const uint8_t {name}_data[{len(data)}] = {{")
        lines.extend(format_bytes(data))
        lines.extend([
            "};",
            "",
            f"static const bitmap_font_t {name} = {{",
            f"    .width = {width},",
            f"    .height = {height},",
            f"    .stride = {stride},",
            f"    .data = {name}_data,",
            "};",
            "",
        ])
        fonts.append(name)

    lines.extend([
        "static const bitmap_font_t *const s_share_tech_mono_fonts[] = {",
        *(f"    &{name}," for name in fonts),
        "};",
        "",
    ])
    OUTPUT.write_text("\n".join(lines), encoding="ascii")


if __name__ == "__main__":
    main()
