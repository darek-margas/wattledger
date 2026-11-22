"""Annotated figures for docs/vue3-hardware.md.

Usage: python -I figures.py <images dir> <output dir>
Coordinates are in the original photos' pixels.
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

SRC, OUT = sys.argv[1], sys.argv[2]
RED = (214, 32, 26)
WIDTH = 1600
BOLD = "C:/Windows/Fonts/segoeuib.ttf"
REG = "C:/Windows/Fonts/segoeui.ttf"


def font(size, bold=True):
    return ImageFont.truetype(BOLD if bold else REG, size)


def badge(d, x, y, n, r=26):
    d.ellipse((x - r, y - r, x + r, y + r), fill=RED, outline=(255, 255, 255), width=4)
    f = font(int(r * 1.25))
    t = str(n)
    w = d.textlength(t, font=f)
    d.text((x - w / 2, y - r * 0.82), t, font=f, fill=(255, 255, 255))


def figure(name, photo, crop, blur=(), rings=(), legend=(), box=None, rotate=0):
    """rings: (x, y, radius, number or None, badge dx, badge dy) in original pixels."""
    im = Image.open(os.path.join(SRC, photo)).convert("RGB")
    if rotate:
        im = im.rotate(rotate, expand=True)
    for b in blur:
        im.paste(im.crop(b).filter(ImageFilter.GaussianBlur(22)), b)
    x0, y0, x1, y1 = crop
    k = WIDTH / (x1 - x0)
    im = im.crop(crop).resize((WIDTH, round((y1 - y0) * k)), Image.LANCZOS)
    d = ImageDraw.Draw(im)
    if box:
        bx0, by0, bx1, by1 = [(v - o) * k for v, o in zip(box, (x0, y0, x0, y0))]
        d.rectangle((bx0, by0, bx1, by1), outline=RED, width=6)
    for x, y, r, n, dx, dy in rings:
        cx, cy, rr = (x - x0) * k, (y - y0) * k, r * k
        d.ellipse((cx - rr, cy - rr, cx + rr, cy + rr), outline=RED, width=max(5, round(rr / 6)))
        if n is not None:
            bx, by = cx + dx * k, cy + dy * k
            d.line((cx + (bx - cx) * rr / max(1, ((bx - cx) ** 2 + (by - cy) ** 2) ** 0.5),
                    cy + (by - cy) * rr / max(1, ((bx - cx) ** 2 + (by - cy) ** 2) ** 0.5), bx, by),
                   fill=RED, width=4)
            badge(d, bx, by, n)
    if legend:
        f = font(34, bold=False)
        line_h = 58
        panel = Image.new("RGB", (WIDTH, 30 + line_h * len(legend)), (255, 255, 255))
        pd = ImageDraw.Draw(panel)
        for i, (n, text) in enumerate(legend):
            y = 20 + i * line_h
            if n is not None:
                badge(pd, 50, y + 24, n, r=22)
            pd.text((95, y), text, font=f, fill=(30, 30, 30))
        out = Image.new("RGB", (WIDTH, im.height + panel.height), (255, 255, 255))
        out.paste(im, (0, 0))
        out.paste(panel, (0, im.height))
        im = out
    im.save(os.path.join(OUT, name), "JPEG", quality=86, optimize=True)  # no EXIF
    print(name, im.size)


os.makedirs(OUT, exist_ok=True)

# 1. Underside overview, flashing area boxed.
figure("01-underside-overview.jpg", "40.jpg", (60, 380, 1990, 1300),
       blur=[(395, 480, 475, 545)],
       box=(1360, 735, 1510, 895),
       legend=[(None, "Underside of the Vue 3 board (P/N 501-1-02649_B3). Mains side left of the yellow line."),
               (None, "Everything you solder is inside the red box, on the low-voltage side.")])

# 2. Flashing pads, close up.
pads = [
    (1453, 797, 7, 1, 22, -10),   # 3.3 V / TP12
    (1453, 770, 7, 2, 22, -14),   # GND / TP5
    (1432, 823, 7, 3, -24, 6),    # TXD / TP4
    (1452, 866, 7, 4, -6, 26),    # RXD / TP3
    (1387, 813, 9, 5, -26, -8),   # IO0 hole
    (1479, 866, 7, 6, 10, 26),    # TP6
]
figure("02-flashing-pads.jpg", "40.jpg", (1340, 720, 1530, 910), rings=pads,
       legend=[(1, "3.3 V (TP12): 3.3 V supply, 500 mA or more"),
               (2, "GND (TP5): adapter GND and supply GND"),
               (3, "TXD (TP4): adapter TX, straight through, not crossed"),
               (4, "RXD (TP3): adapter RX"),
               (5, "IO0 (plated hole): GND before power-up, for download mode"),
               (6, "TP6 (SAMD09 reset): GND while reading or flashing; remove before ESPHome boots")])

# 3. RTC power from the same pads (wires soldered).
figure("03-rtc-power-pads.jpg", "27.jpg", (700, 330, 1500, 1010),
       rings=[(1222, 515, 28, 1, 120, -40), (1222, 590, 28, 2, 130, 40)],
       legend=[(1, "GND (TP5) to PiicoDev GND"),
               (2, "3.3 V (TP12) to PiicoDev 3.3V"),
               (None, "The other soldered spots are left over from flashing; the wires there are gone.")])

# 4. Power wires taken through to the top side.
figure("04-power-wires-through.jpg", "25.jpg", (1000, 300, 1800, 1000),
       rings=[(1440, 565, 22, 1, 80, -50), (1440, 843, 22, 2, 80, 40), (1492, 737, 72, 3, 110, 90)],
       legend=[(1, "GND wire through a spare hole to the top side"),
               (2, "3.3 V wire through a spare hole to the top side"),
               (3, "U18, the RTL8201F Ethernet PHY (not touched)")])

# 5. SDA and SCL on the WROVER-IE castellations.
figure("05-wrover-pins-29-30.jpg", "28.jpg", (200, 560, 1500, 1260),
       blur=[(1080, 1150, 1245, 1262)],
       rings=[(850, 1017, 22, 1, 70, -120), (802, 1017, 22, 2, -70, -120),
              (405, 1017, 22, 3, -10, -110), (1290, 1017, 22, 4, 10, -110)],
       legend=[(1, "Pin 29 = GPIO5 = SDA (10th from the antenna end)"),
               (2, "Pin 30 = GPIO18 = SCL (9th from the antenna end)"),
               (3, "Pin 38, antenna end (printed \"38\")"),
               (4, "Pin 20, far end (printed \"20\")")])

# 6. PiicoDev back: cut the I2C pull-up jumper.
figure("06-piicodev-i2c-pu.jpg", "10.jpg", (640, 500, 1280, 1120),
       rings=[(1212, 978, 45, 1, -30, 100)],
       legend=[(1, "I2C PU: three pads joined by two thin traces; cut both"),
               (None, "Then SDA and SCL to 3.3V read open (the Vue has its own pull-ups).")])

# 7. RTC wired and mounted (top side).
figure("07-rtc-wired.jpg", "38.jpg", (300, 380, 1100, 1000),
       rings=[(628, 868, 14, 1, 70, 40), (628, 836, 14, 2, 80, 10),
              (628, 805, 14, 3, 80, -20), (628, 775, 14, 4, 70, -50)],
       legend=[(1, "GND from TP5"), (2, "3.3V from TP12"),
               (3, "SDA from WROVER pin 29"), (4, "SCL from WROVER pin 30"),
               (None, "EVI, CLK and INT stay unconnected. Tape under the board, on the module shield.")])

# 8. Final assembly in the case.
figure("08-final-in-case.jpg", "36.jpg", (60, 170, 1940, 1070),
       blur=[(395, 335, 715, 475), (540, 440, 645, 570)],
       legend=[(None, "Finished: RTC on the module shield, wires kept on the low-voltage side.")])

# 9. The wire.
figure("09-wire.jpg", "32.jpg", (250, 250, 1800, 1300),
       legend=[(None, "Thin PTFE-insulated plated copper wire: doesn't melt or scratch at the iron.")])
