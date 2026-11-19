"""Draw the bundled app mark and pack native Win32 icon images."""

import struct

from PIL import Image, ImageDraw


def app_mark(size):
    image = Image.new("RGBA", (256, 256))
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((3, 3, 252, 252), radius=58, fill="#173044")
    draw.rounded_rectangle((8, 8, 247, 247), radius=53, outline="#426375", width=3)
    draw.polygon([(48, 128), (52, 54), (104, 88), (151, 88), (205, 54), (208, 128)], fill="#F5FAFC")
    draw.rounded_rectangle((48, 90, 208, 200), radius=53, fill="#F5FAFC")
    draw.polygon([(62, 98), (64, 74), (89, 93)], fill="#A6D8D5")
    draw.polygon([(167, 93), (192, 74), (194, 98)], fill="#A6D8D5")
    draw.line((75, 130, 65, 142, 75, 154), fill="#1B4B55", width=9, joint="curve")
    draw.line((181, 130, 191, 142, 181, 154), fill="#1B4B55", width=9, joint="curve")
    draw.ellipse((98, 132, 112, 148), fill="#173044")
    draw.ellipse((143, 132, 157, 148), fill="#173044")
    draw.ellipse((123, 150, 133, 159), fill="#173044")
    draw.polygon([(99, 178), (128, 189), (157, 178), (157, 211), (128, 200), (99, 211)], fill="#E1505C")
    draw.rounded_rectangle((120, 186, 136, 203), radius=5, fill="#B9364A")
    return image.resize((size, size), Image.Resampling.LANCZOS)


def icon_resources():
    entries = []
    resources = []
    for image_id, size in enumerate((16, 24, 32, 48, 64), 1):
        image = app_mark(size).transpose(Image.Transpose.FLIP_TOP_BOTTOM)
        pixels = image.tobytes("raw", "BGRA")
        stride = ((size + 31) // 32) * 4
        mask = bytearray(stride * size)
        alpha = image.getchannel("A").tobytes()
        for y in range(size):
            for x in range(size):
                if not alpha[y * size + x]:
                    mask[y * stride + x // 8] |= 1 << (7 - x % 8)
        dib = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, len(pixels), 0, 0, 0, 0)
        payload = dib + pixels + mask
        entries.append(struct.pack("<BBBBHHIH", size, size, 0, 0, 1, 32, len(payload), image_id))
        resources.append((3, image_id, 0, payload))
    group = struct.pack("<HHH", 0, 1, len(entries)) + b"".join(entries)
    return resources + [(14, 1, 0, group)]
