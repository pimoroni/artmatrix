import time


def run():
    SPACING = 7

    WIDTH = screen.width
    HEIGHT = screen.height
    screen.antialias = image.X4

    am_logo = image.load("/assets/artmatrix_logo.png")

    dot_pos = [(x * SPACING, y * SPACING, (x * SPACING) / WIDTH, ((x * SPACING) * 2) % 255) for x in range((WIDTH - 1) // SPACING) for y in range((HEIGHT - 1) // SPACING)]
    crisp = image(WIDTH, HEIGHT)
    last_frame = time.ticks_ms()
    frame = 0

    FADE = 1.0
    BLUR = 0.6

    while frame < FADE + 1.7:
        screen.pen = color.rgb(0, 0, 0)
        screen.clear()

        now = time.ticks_ms()
        dt = time.ticks_diff(now, last_frame)
        last_frame = now
        frame += dt / 500
        sweep = frame - FADE

        for dot in dot_pos:
            x, y, unit, hue = dot
            dot_lit = max(0, 1 - abs(sweep - unit) * 8)
            screen.pen = color.hsv(hue, int(255 - dot_lit * 160), int(max(0.4, dot_lit) * 255))
            screen.rectangle(x + 2, y + 2, 5, 5)

        blur_in = min(1.0, frame / BLUR)
        if blur_in < 1.0:
            crisp.blit(screen, vec2(0, 0))

        screen.blur(0.1)

        if blur_in < 1.0:
            screen.alpha = int((1.0 - blur_in) * 255)
            screen.blit(crisp, vec2(0, 0))
            screen.alpha = 255

        logo_alpha = min(1.0, frame / FADE)
        screen.alpha = int(logo_alpha * 160)
        screen.blit(am_logo, rect(32, 32, 64, 64))
        screen.alpha = 255

        display.update()

    result = image(screen.width, screen.height)
    result.blit(screen, vec2(0, 0))
    return result
