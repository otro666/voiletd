# -*- coding: utf-8 -*-
# Генератор сглаженных шрифтов для платы.
#
# LovyanGFX умеет грузить шрифты формата VLW (тот же, что у «smooth fonts» TFT_eSPI и
# Processing): картинка каждой буквы хранится с 8-битной прозрачностью, и библиотека
# рисует её со сглаживанием, смешивая цвет буквы с цветом фона. Встроенные efont такого
# не умеют — они побитовые, отсюда и «лесенки».
#
# Скрипт растрирует DejaVu Sans (латиница + кириллица) в три размера и складывает их
# массивами в src/fonts_vlw.cpp. Запускать при смене размеров или набора символов:
#   python3 tools/make_vlw.py
from PIL import Image, ImageDraw, ImageFont
import struct, os, sys

OUT_CPP = os.path.join(os.path.dirname(__file__), "..", "src", "fonts_vlw.cpp")
OUT_H   = os.path.join(os.path.dirname(__file__), "..", "include", "fonts_vlw.h")

REG  = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
BOLD = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"

# Набор символов: ASCII, ВСЯ кириллица с Ё/ё, типографика и ходовые знаки.
#
# Список ОБЯЗАТЕЛЬНО отсортирован по кодам: библиотека ищет букву двоичным поиском по
# таблице, и неупорядоченная таблица ломает поиск — буквы есть в шрифте, а на экране их
# нет. Именно так в первой версии пропала половина кириллицы: Ё и знаки лежали в конце,
# после а-я, и рушили порядок.
def charset():
    cs = set(chr(c) for c in range(0x20, 0x7F))            # ASCII с цифрами и знаками
    cs |= set(chr(c) for c in range(0x0410, 0x0450))       # А-Я а-я
    cs |= set("Ёё")
    cs |= set("«»°±·×÷№₽✓")
    cs |= set("‘’“”„–—…")
    return sorted(cs, key=ord)

# Все 33 буквы алфавита в обоих регистрах — иначе шрифт не годен.
ALPHABET = "АБВГДЕЁЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЫЬЭЮЯ"
def check_alphabet(chars):
    have = set(chars)
    missing = [c for c in ALPHABET + ALPHABET.lower() if c not in have]
    assert not missing, f"в наборе нет букв: {missing}"

def build(path, px):
    font = ImageFont.truetype(path, px)
    ascent, descent = font.getmetrics()
    chars = charset()
    check_alphabet(chars)
    glyphs = []
    for ch in chars:
        adv = int(round(font.getlength(ch)))
        bbox = font.getbbox(ch)
        if bbox is None or bbox[2] <= bbox[0] or bbox[3] <= bbox[1]:
            glyphs.append((ord(ch), 0, 0, adv, 0, 0, b""))
            continue
        x0, y0, x1, y1 = bbox
        w, h = x1 - x0, y1 - y0
        img = Image.new("L", (w + 8, h + 8), 0)
        ImageDraw.Draw(img).text((4 - x0, 4 - y0), ch, font=font, fill=255)
        bmp = img.crop((4, 4, 4 + w, 4 + h)).tobytes()
        # gdY — от базовой линии ВВЕРХ до верхнего края картинки, gdX — вбок.
        glyphs.append((ord(ch), h, w, adv, ascent - y0, x0, bmp))

    # Заголовок и таблица — числа старшим байтом вперёд, как требует формат.
    out = struct.pack(">6i", len(glyphs), 11, ascent + descent, 0, ascent, descent)
    for u, h, w, adv, dy, dx, _ in glyphs:
        out += struct.pack(">7i", u, h, w, adv, dy, dx, 0)
    for *_, bmp in glyphs:
        out += bmp
    return out

def selfcheck(blob):
    # Разбираем то, что записали, тем же способом, каким читает библиотека, — расхождение
    # формата дешевле поймать здесь, чем на плате.
    n, ver, size, _, asc, desc = struct.unpack_from(">6i", blob, 0)
    assert ver == 11 and 0 < n < 1000 and asc > 0 and desc >= 0
    off = 24; total = 0; prev = -1
    for i in range(n):
        u, h, w, adv, dy, dx, pad = struct.unpack_from(">7i", blob, off + i * 28)
        assert pad == 0 and 0 <= w < 64 and 0 <= h < 64 and 0 < adv < 64
        assert u > prev, "таблица не отсортирована — двоичный поиск букв сломается"
        prev = u
        total += w * h
    assert len(blob) == 24 + n * 28 + total
    return n, size

def emit(name, blob, f):
    f.write(f"const uint8_t {name}[{len(blob)}] = {{\n")
    for i in range(0, len(blob), 24):
        f.write("  " + ",".join(str(b) for b in blob[i:i+24]) + ",\n")
    f.write("};\n\n")

def main():
    fonts = [
        ("vualFontSmall", REG, 13),   # служебные строки
        ("vualFontText",  REG, 17),   # текст сообщений и списков
        ("vualFontTitle", BOLD, 16),  # заголовки
    ]
    blobs = []
    for name, path, px in fonts:
        b = build(path, px)
        n, size = selfcheck(b)
        print(f"{name}: {px}px, {n} глифов, {len(b)} байт, строка {size}px")
        blobs.append((name, b))

    with open(OUT_H, "w") as f:
        f.write(
            "// Сглаженные шрифты (формат VLW) с кириллицей. Файл СГЕНЕРИРОВАН\n"
            "// скриптом tools/make_vlw.py — руками не править, править скрипт.\n"
            "#pragma once\n#include <stdint.h>\n\n")
        for name, b in blobs:
            f.write(f"extern const uint8_t {name}[{len(b)}];\n")

    with open(OUT_CPP, "w") as f:
        f.write(
            "// Сглаженные шрифты (формат VLW). Файл СГЕНЕРИРОВАН tools/make_vlw.py.\n"
            "#include \"fonts_vlw.h\"\n\n")
        for name, b in blobs:
            emit(name, b, f)
    print("записано:", OUT_CPP)

if __name__ == "__main__":
    sys.exit(main())
