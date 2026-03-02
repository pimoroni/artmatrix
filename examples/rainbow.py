import time

WIDTH = 128
HEIGHT = 128
STRIPES = 0.5


@micropython.native  # noqa: F821
def draw(offset):
    for x in range(WIDTH):
        screen.pen = color.oklch(200, 255, (x / WIDTH * STRIPES * 255) + offset)
        screen.line(x, 0, x, HEIGHT)

    display.update()


t_total = 0
t_frames = 0

while True:
    t_start = time.ticks_ms()
    draw(time.ticks_ms() / 50)
    t_total += time.ticks_diff(time.ticks_ms(), t_start)
    t_frames += 1

    if t_frames == 100:
        per_frame_avg = t_total / t_frames
        print(f"100 frames in {t_total}ms, avg {per_frame_avg:.02f}ms per frame, {1000 / per_frame_avg:.02f} FPS")
        t_frames = 0
        t_total = 0
