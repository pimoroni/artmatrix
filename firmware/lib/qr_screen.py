import math
import time

import config

# built once per address, keyed by it
qr_codes = {}


def draw_qr_screen(title, hint, plugin=None):
    # pen and font belong to whoever called, so they go back afterwards
    pen, font = screen.pen, screen.font

    screen.pen = color.rgb(0, 0, 0)
    screen.clear()

    # slow hue drift on the text and corners, the qr stays white for contrast
    now = time.ticks_ms()
    screen.pen = color.oklch(190, 255, (now // 40) % 256).fit()
    screen.font = rom_font.nope

    address = f"http://{config.wifi_hostname}.local"
    if plugin:
        address += f"/config?plugin={plugin}"
    if address not in qr_codes:
        qr = image.qr(address, image.QR_LOW, 0)
        # qr() draws dark on light, so swap the palette like the setup screen
        qr.palette = [color.rgb(0, 0, 0), color.white]
        qr_codes[address] = qr
    qr = qr_codes[address]
    # 3px a square, or 2px past 29 squares, where a long address would fill the screen
    qr_size = qr.width * (3 if qr.width <= 29 else 2)
    qr_x = (screen.width - qr_size) // 2
    qr_y = (screen.height - qr_size) // 2
    screen.blit(qr, rect(qr_x, qr_y, qr_size, qr_size))

    # the title sits up to 5px from the top, closer when a plugin page's
    # bigger qr needs the room
    title_width, title_height = screen.measure_text(title)
    title_y = max(0, min(5, (screen.height - qr_size - 2 * title_height) // 4))
    screen.text(title, (screen.width - title_width) // 2, title_y)
    gap = qr_y - (title_y + title_height)

    # viewfinder corners breathing 3-5px out from the qr, ~2s a cycle, on the
    # main screen only and where the gap leaves room
    if not plugin and gap >= 7:
        offset = 4 + math.sin(now / 320)
        arm = 12
        outer = qr_size + (offset + 2) * 2
        left = qr_x - offset - 2
        top = qr_y - offset - 2
        for x in (left, left + outer - arm):
            for y in (top, top + outer - 2):
                screen.shape(shape.rectangle(x, y, arm, 2))
        for x in (left, left + outer - 2):
            for y in (top, top + outer - arm):
                screen.shape(shape.rectangle(x, y, 2, arm))

    # same gap below the qr as between the title and the qr
    screen.text(hint, (screen.width - screen.measure_text(hint)[0]) // 2, qr_y + qr_size + gap)

    screen.pen, screen.font = pen, font
