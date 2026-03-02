import gc
import time
import random
from ulab import numpy

width = 128
height = 128

"""
HELLO NEO.
"""


def update():
    for _ in range(4):
        x = random.randint(0, width - 1)
        y = random.randint(0, height - 20)
        m = random.randint(30, 127)
        for oy in range(random.randint(3, 7)):
            matrix[y + oy][x] = random.randint(0, m) / 255.0
        matrix[y + 10][x] = random.randint(128, 255) / 255.0

    # Propagate downwards
    old = numpy.ndarray(matrix)
    matrix[:] = numpy.roll(matrix, 1, axis=0)
    matrix[:] *= 0.59
    matrix[:] += old * 0.39

    out[0::4] = out[1::4] = out[2::4] = numpy.ndarray(numpy.where(matrix > 0.5, 1.0, 0.0) * 128, dtype=numpy.uint8).reshape((1, width * height))
    out[1::4] += numpy.ndarray(numpy.clip(matrix, 0, 0.5) * 128, dtype=numpy.uint8).reshape((1, width * height))


def draw():
    # Copy the effect to the framebuffer
    memoryview(display)[:] = out.tobytes()



matrix = numpy.zeros((height, width))
out = numpy.zeros(width * height * 4, dtype=numpy.uint8)


t_count = 0
t_total = 0


while True:
    tstart = time.ticks_ms()
    gc.collect()
    update()
    draw()
    display.update()
    tfinish = time.ticks_ms()

    total = tfinish - tstart
    t_total += total
    t_count += 1

    if t_count == 60:
        per_frame_avg = t_total / t_count
        print(f"60 frames in {t_total}ms, avg {per_frame_avg:.02f}ms per frame, {1000/per_frame_avg:.02f} FPS")
        t_count = 0
        t_total = 0

    # pause for a moment (important or the USB serial device will fail)
    # try to pace at 60fps or 30fps
    if total > 1000 / 30:
        time.sleep(0.0001)
    elif total > 1000 / 60:
        t = 1000 / 30 - total
        time.sleep(t / 1000)
    else:
        t = 1000 / 60 - total
        time.sleep(t / 1000)
