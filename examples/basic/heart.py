import time
import math
from ulab import numpy
import easing

POINTS = 100
AMPLITUDE = 18

screen.antialias = image.X4

t = numpy.linspace(0, 2 * numpy.pi, POINTS)
x = AMPLITUDE * numpy.sin(t) ** 3
y = AMPLITUDE * 0.8 * numpy.cos(t) - 5 * numpy.cos(2 * t) - 2 * numpy.cos(3 * t) - numpy.cos(4 * t)
y -= (abs(max(y)) - abs(min(y))) / 2  # Vertical center align
y *= -1  # Vertical flip
x /= AMPLITUDE
y /= AMPLITUDE

heart = shape.custom([vec2(*p) for p in zip(x, y)])


while True:
    scale = (math.sin(time.ticks_ms() / 80) + 1.0) / 2.0
    scale = easing.easeInOutQuad(scale)
    scale *= 5
    scale += 50
    # Our heart is unit sized at 0,0; transform and scale it
    heart.transform = mat3().translate(64, 64).scale(scale)

    screen.pen = color.black
    screen.clear()

    screen.pen = color.red
    screen.shape(heart)

    display.update()

