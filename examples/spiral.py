import math
import time


WIDTH = 128
HEIGHT = 128

# Clear the screen
screen.pen = color.black
screen.clear()

# Set the initial pen colour
screen.pen = color.oklch(255, 255, 255)

n = 0
c = 2

while True:

    # We'll use this for cycling through the rainbow
    t = time.ticks_ms() / 1000

    a = n * 140
    r = c * math.sqrt(n)

    x = int(r * math.cos(a) + WIDTH // 2)
    y = int(r * math.sin(a) + HEIGHT // 2)

    screen.circle(x, y, 1)

    # Update the display
    display.update()

    n += 1

    # The value '1600' is roughly the number of iterations required to fill a 128 x 128 matrix
    if n > 1600:
        # Clear the screen
        screen.pen = color.black
        screen.clear()

        # Set the new colour based on the ticks
        screen.pen = color.oklch(255, 255, t * 255)

        # Set variable n back to 0 to start the spiral again
        n = 0
