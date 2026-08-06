# -*- coding: utf-8 -*-
# Генератор встроенных эмодзи со сглаживанием.
#
# Прежние встроенные рисунки хранили один бит прозрачности на точку — края выходили
# ступенчатыми, и никакое смягчение на плате этого не лечило. Здесь каждый рисунок
# рисуется вчетверо крупнее (112×112) и сжимается фильтром до 28×28: плавная
# прозрачность получается сама собой.
#
# Хранится так же, как развёрнутые картинки с карты: цвет, уже домноженный на
# прозрачность (в формате экрана), плюс прозрачность отдельным байтом. Смешивание с
# любой подложкой — одно сложение на канал.
#
# Запуск: python3 tools/make_emoji.py  →  src/emoji_rom.cpp, include/emoji_rom.h
from PIL import Image, ImageDraw
import os, math

S = 112           # холст
OUT = 32          # итог — как emoji::kSize
OUT_CPP = os.path.join(os.path.dirname(__file__), "..", "src", "emoji_rom.cpp")
OUT_H   = os.path.join(os.path.dirname(__file__), "..", "include", "emoji_rom.h")

FACE   = (255, 202, 44, 255)
RIM    = (232, 166, 31, 255)
DARK   = (66, 46, 26, 255)
WHITE  = (255, 255, 255, 255)
RED    = (233, 62, 46, 255)
BLUE   = (66, 165, 245, 255)
PINK   = (245, 130, 130, 255)
SKIN   = (245, 198, 138, 255)
SKIN_D = (222, 168, 104, 255)
BROWN  = (150, 100, 62, 255)
BROWN_D= (120, 78, 46, 255)
GOLD   = (255, 213, 74, 255)
GOLD_D = (232, 180, 42, 255)

def canvas():
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    return img, ImageDraw.Draw(img)

def face_base(d):
    d.ellipse((6, 6, S - 6, S - 6), fill=FACE, outline=RIM, width=5)

def eyes(d, dy=0, w=10, h=14):
    for cx in (38, 74):
        d.ellipse((cx - w // 2, 40 + dy, cx + w // 2, 40 + dy + h), fill=DARK)

def smile_arc(d, y0=52, y1=92, a0=25, a1=155, wd=6):
    d.arc((26, y0, S - 26, y1), a0, a1, fill=DARK, width=wd)

def closed_happy(d):
    for cx in (38, 74):
        d.arc((cx - 11, 38, cx + 11, 58), 190, 350, fill=DARK, width=6)

def em_smile():
    img, d = canvas(); face_base(d); eyes(d); smile_arc(d); return img

def em_laugh():
    img, d = canvas(); face_base(d); closed_happy(d)
    d.pieslice((30, 56, S - 30, 98), 0, 180, fill=DARK)
    d.pieslice((40, 74, S - 40, 96), 0, 180, fill=(200, 60, 60, 255))
    d.ellipse((6, 60, 26, 88), fill=BLUE)      # слёзы по щекам
    d.ellipse((S - 26, 60, S - 6, 88), fill=BLUE)
    return img

def em_blush():
    img, d = canvas(); face_base(d); closed_happy(d); smile_arc(d, 56, 92)
    d.ellipse((18, 62, 38, 76), fill=PINK)
    d.ellipse((S - 38, 62, S - 18, 76), fill=PINK)
    return img

def heart_shape(d, cx, cy, r, color):
    d.ellipse((cx - r, cy - r, cx, cy), fill=color)
    d.ellipse((cx, cy - r, cx + r, cy), fill=color)
    d.polygon([(cx - r, cy - r * 0.35), (cx + r, cy - r * 0.35), (cx, cy + r)], fill=color)

def em_heart_eyes():
    img, d = canvas(); face_base(d)
    heart_shape(d, 38, 44, 13, RED)
    heart_shape(d, 74, 44, 13, RED)
    d.pieslice((34, 62, S - 34, 96), 0, 180, fill=DARK)
    return img

def em_cool():
    img, d = canvas(); face_base(d)
    d.rounded_rectangle((16, 36, 52, 56), 8, fill=DARK)
    d.rounded_rectangle((60, 36, 96, 56), 8, fill=DARK)
    d.line((14, 40, 20, 40), fill=DARK, width=5)
    d.line((52, 42, 60, 42), fill=DARK, width=5)
    d.line((92, 40, 98, 40), fill=DARK, width=5)
    smile_arc(d, 56, 92)
    return img

def em_think():
    img, d = canvas(); face_base(d)
    d.ellipse((33, 44, 43, 58), fill=DARK)
    d.ellipse((69, 40, 79, 54), fill=DARK)
    d.arc((28, 30, 50, 44), 200, 340, fill=DARK, width=5)   # брови: одна ровная...
    d.arc((64, 22, 90, 38), 200, 340, fill=DARK, width=5)   # ...другая поднята
    d.line((38, 82, 64, 77), fill=DARK, width=6)            # скошенный рот
    d.ellipse((58, 92, 96, 112), fill=SKIN, outline=SKIN_D, width=3)   # кисть у подбородка
    d.rounded_rectangle((62, 84, 74, 100), 6, fill=SKIN, outline=SKIN_D, width=2)
    return img

def em_sad():
    img, d = canvas(); face_base(d)
    for cx in (38, 74):
        d.arc((cx - 11, 44, cx + 11, 60), 10, 170, fill=DARK, width=6)
    d.arc((34, 78, S - 34, 104), 205, 335, fill=DARK, width=6)
    return img

def em_cry():
    img, d = canvas(); face_base(d)
    eyes(d, dy=0, w=10, h=12)
    d.arc((34, 80, S - 34, 106), 205, 335, fill=DARK, width=6)
    d.polygon([(30, 56), (44, 56), (37, 92)], fill=BLUE)    # крупная слеза
    d.ellipse((28, 82, 46, 102), fill=BLUE)
    return img

def em_angry():
    img, d = canvas()
    d.ellipse((6, 6, S - 6, S - 6), fill=(236, 92, 44, 255), outline=(198, 60, 26, 255), width=5)
    d.line((24, 36, 48, 48), fill=DARK, width=6)            # сведённые брови
    d.line((88, 36, 64, 48), fill=DARK, width=6)
    d.ellipse((34, 50, 46, 64), fill=DARK)
    d.ellipse((66, 50, 78, 64), fill=DARK)
    d.arc((34, 80, S - 34, 108), 200, 340, fill=DARK, width=6)
    return img

def em_kiss():
    img, d = canvas(); face_base(d)
    d.ellipse((33, 42, 43, 56), fill=DARK)                  # открытый глаз
    d.arc((63, 42, 85, 56), 190, 350, fill=DARK, width=6)   # подмигивание
    d.ellipse((48, 70, 66, 92), fill=(216, 90, 80, 255))    # губы трубочкой
    heart_shape(d, 84, 84, 10, RED)
    return img

def em_heart():
    img, d = canvas()
    heart_shape(d, 56, 52, 34, RED)
    d.ellipse((32, 32, 44, 42), fill=(255, 140, 128, 255))  # блик — узкий, у плеча
    d.ellipse((36, 34, 44, 41), fill=RED)
    return img

def em_thumb():
    img, d = canvas()
    d.rounded_rectangle((18, 56, 44, 100), 8, fill=(74, 122, 216, 255))   # манжета
    d.rounded_rectangle((40, 52, 92, 102), 14, fill=SKIN, outline=SKIN_D, width=4)
    d.rounded_rectangle((52, 16, 72, 64), 10, fill=SKIN, outline=SKIN_D, width=4)  # палец
    for y in (66, 78, 90):
        d.line((44, y, 90, y), fill=SKIN_D, width=3)
    return img

def em_ok():
    img, d = canvas()
    # Три пальца веером вверх, под ними колечко из большого и указательного.
    for x0, y0, x1, y1 in ((44, 10, 58, 62), (62, 14, 76, 66), (78, 26, 92, 72)):
        d.rounded_rectangle((x0, y0, x1, y1), 7, fill=SKIN, outline=SKIN_D, width=3)
    d.ellipse((20, 50, 68, 98), fill=SKIN)
    d.ellipse((32, 62, 56, 86), fill=(0, 0, 0, 0))          # прорезь колечка
    d.ellipse((20, 50, 68, 98), outline=SKIN_D, width=4)
    d.ellipse((32, 62, 56, 86), outline=SKIN_D, width=4)
    return img

def em_clap():
    img, d = canvas()
    for x0, y0, x1, y1 in ((52, 8, 60, 26), (28, 14, 40, 30), (74, 14, 86, 30)):
        d.line((x0 + (x1 - x0) // 2, y0, x0 + (x1 - x0) // 2, y1)
               if x1 - x0 < 10 else (x0, y1, x1, y0), fill=GOLD, width=6)   # искры хлопка
    # Две ладони наклонены навстречу, пальцы намечены бороздками.
    d.rounded_rectangle((20, 40, 56, 106), 16, fill=SKIN, outline=SKIN_D, width=4)
    d.rounded_rectangle((58, 40, 94, 106), 16, fill=SKIN_D, outline=BROWN, width=4)
    for y in (56, 70, 84):
        d.line((24, y, 52, y), fill=SKIN_D, width=3)
        d.line((62, y, 90, y), fill=BROWN, width=3)
    return img

def em_fire():
    img, d = canvas()
    d.polygon([(56, 6), (86, 44), (92, 76), (78, 100), (34, 100), (20, 74), (30, 42)],
              fill=(240, 120, 32, 255))
    d.ellipse((22, 48, 90, 108), fill=(240, 120, 32, 255))
    d.polygon([(56, 40), (74, 66), (76, 86), (56, 100), (36, 86), (40, 64)],
              fill=GOLD)
    d.ellipse((38, 68, 74, 106), fill=GOLD)
    d.ellipse((46, 80, 66, 104), fill=(255, 244, 180, 255))
    return img

def em_poop():
    img, d = canvas()
    d.ellipse((18, 62, 94, 106), fill=BROWN, outline=BROWN_D, width=4)
    d.ellipse((28, 40, 84, 84), fill=BROWN, outline=BROWN_D, width=4)
    d.ellipse((40, 22, 72, 56), fill=BROWN, outline=BROWN_D, width=4)
    d.ellipse((40, 74, 54, 88), fill=WHITE)
    d.ellipse((60, 74, 74, 88), fill=WHITE)
    d.ellipse((44, 78, 51, 86), fill=DARK)
    d.ellipse((64, 78, 71, 86), fill=DARK)
    d.arc((42, 84, 72, 100), 20, 160, fill=DARK, width=4)
    return img

def em_star():
    img, d = canvas()
    pts = []
    for i in range(10):
        r = 50 if i % 2 == 0 else 21
        a = math.radians(-90 + i * 36)
        pts.append((56 + r * math.cos(a), 58 + r * math.sin(a)))
    d.polygon(pts, fill=GOLD, outline=GOLD_D)
    return img

# Порядок СТРОГО как в emoji_data.cpp — индексы совпадают с kSet.
EMOJI = [
    ("1F642", em_smile), ("1F602", em_laugh), ("1F60A", em_blush),
    ("1F60D", em_heart_eyes), ("1F60E", em_cool), ("1F914", em_think),
    ("1F614", em_sad), ("1F622", em_cry), ("1F621", em_angry),
    ("1F618", em_kiss), ("2764", em_heart), ("1F44D", em_thumb),
    ("1F44C", em_ok), ("1F44F", em_clap), ("1F525", em_fire),
    ("1F4A9", em_poop), ("2B50", em_star),
]

def to565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)

def main():
    prem_all, alpha_all, previews = [], [], []
    for name, fn in EMOJI:
        img = fn().resize((OUT, OUT), Image.LANCZOS)
        previews.append(img)
        prem, alpha = [], []
        for py in range(OUT):
            for px in range(OUT):
                r, g, b, a = img.getpixel((px, py))
                # Цвет заранее домножен на прозрачность: на плате остаётся одно сложение.
                prem.append(to565(r * a // 255, g * a // 255, b * a // 255))
                alpha.append(a)
        prem_all.append((name, prem)); alpha_all.append((name, alpha))

    with open(OUT_H, "w") as f:
        f.write("// Встроенные эмодзи со сглаживанием. СГЕНЕРИРОВАНО tools/make_emoji.py.\n"
                "#pragma once\n#include <stdint.h>\n\nnamespace emoji {\n\n"
                "/** Цвет, домноженный на прозрачность (формат экрана), и прозрачность.\n"
                " *  Ровно то же представление, что у картинок, развёрнутых с карты. */\n"
                "struct RomEmoji { const uint16_t* premul; const uint8_t* alpha; };\n\n"
                f"extern const RomEmoji kRom[{len(EMOJI)}];\n\n}}  // namespace emoji\n")

    with open(OUT_CPP, "w") as f:
        f.write("// СГЕНЕРИРОВАНО tools/make_emoji.py — править сценарий, не файл.\n"
                "#include \"emoji_rom.h\"\n\nnamespace emoji {\n\n")
        for name, prem in prem_all:
            f.write(f"static const uint16_t kP_{name}[{OUT*OUT}] = {{\n")
            for i in range(0, len(prem), 14):
                f.write("  " + ",".join(f"0x{v:04X}" for v in prem[i:i+14]) + ",\n")
            f.write("};\n")
        for name, alpha in alpha_all:
            f.write(f"static const uint8_t kA_{name}[{OUT*OUT}] = {{\n")
            for i in range(0, len(alpha), 24):
                f.write("  " + ",".join(str(v) for v in alpha[i:i+24]) + ",\n")
            f.write("};\n")
        f.write(f"\nconst RomEmoji kRom[{len(EMOJI)}] = {{\n")
        for name, _ in prem_all:
            f.write(f"    {{kP_{name}, kA_{name}}},\n")
        f.write("};\n\n}  // namespace emoji\n")

    # Превью для глаз: сетка на тёмном фоне переписки.
    grid = Image.new("RGBA", (6 * 40, 3 * 40), (11, 7, 16, 255))
    for i, im in enumerate(previews):
        grid.paste(im, (6 + (i % 6) * 40, 6 + (i // 6) * 40), im)
    grid.resize((grid.width * 3, grid.height * 3), Image.NEAREST).save(
        os.path.join(os.path.dirname(__file__), "emoji_preview.png"))
    print(f"эмодзи: {len(EMOJI)}, файл {os.path.getsize(OUT_CPP)} байт")

if __name__ == "__main__":
    main()
