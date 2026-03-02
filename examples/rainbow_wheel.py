# A spinny rainbow wheel. Change up some of the constants below to see what happens.

import math

# Constants for drawing
INNER_RADIUS = 25
OUTER_RADIUS = 200
NUMBER_OF_LINES = 30
HUE_SHIFT = 0.01
ROTATION_SPEED = 0.4

# Set up the display
WIDTH, HEIGHT = 128, 129

BLACK = color.rgb(0, 0, 0)

# Variables to keep track of rotation and hue positions
r = 0
t = 0
pens = [0 for _ in range(NUMBER_OF_LINES)]

display.set_blocking(False)

while True:
    # Calculate our new pen values while the display is copying
    for i in range(NUMBER_OF_LINES):
        pens[i] = color.oklch(200, 255, ((i / 360) + t) * 100)

    # Wait for the display to finish copying
    while display.is_busy():
        pass

    screen.pen = BLACK
    screen.clear()

    for i in range(NUMBER_OF_LINES):
        screen.pen = pens[i]
        a = 360 * i / NUMBER_OF_LINES
        # Draw some lines, offset by the rotation variable
        screen.line(WIDTH / 2 + math.cos(math.radians(a + r)) * INNER_RADIUS,
                      HEIGHT / 2 + math.sin(math.radians(a + r)) * INNER_RADIUS,
                      WIDTH / 2 + math.cos(math.radians(a + 90 + r)) * OUTER_RADIUS,
                      HEIGHT / 2 + math.sin(math.radians(a + 90 + r)) * OUTER_RADIUS)
    display.update()
    r += ROTATION_SPEED
    t += HUE_SHIFT
