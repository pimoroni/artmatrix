import random

width = 128
height = 128


class Ball:
    def __init__(self, x, y, r, dx, dy, pen, a):
        self.x = x
        self.y = y
        self.r = r
        self.dx = dx
        self.dy = dy
        self.pen = pen
        self.a = a


# initialise shapes
balls = []
for _ in range(75):
    r = random.randint(0, 3) + 3
    balls.append(
        Ball(
            random.randint(r, r + (width - 2 * r)),
            random.randint(r, r + (height - 2 * r)),
            r,
            (7 - r) / 4,
            (7 - r) / 4,
            color.rgb(random.randint(0, 255), random.randint(0, 255), random.randint(0, 255), random.randint(100, 255)),
            random.randint(0, 360)
        )
    )

BG = color.rgb(0, 0, 0)

# Create a unit circle of radius 1
circle = shape.circle(0, 0, 1.0)

# Or... a star
# circle = shape.star(0, 0, 5, 1.0, 0.5, 3)


while True:
    for ball in balls:
        ball.x += ball.dx
        ball.y += ball.dy

        xmax = width - ball.r
        xmin = ball.r
        ymax = height - ball.r
        ymin = ball.r

        if ball.x < xmin or ball.x > xmax:
            ball.dx *= -1

        if ball.y < ymin or ball.y > ymax:
            ball.dy *= -1

    # Wait before clearing the screen
    # we (Core1) might still be copying the buffer
    while display.is_busy():
        pass

    screen.pen = BG
    screen.clear()

    for ball in balls:
        screen.pen = ball.pen
        # Scale and translate our unit circle
        circle.transform = mat3().translate(ball.x, ball.y).scale(ball.r).rotate(ball.a)
        screen.shape(circle)

    display.update()
