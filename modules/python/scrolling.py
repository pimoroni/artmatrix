# Draw scrolling text into a given window

import time


class Scroll:
    def __init__(self, text, font_face=None, font_size=None, target=None, speed=25, gap=None, align="middle"):
        self.text = text
        self.font_face = font_face or font.sins
        self.font_size = font_size
        self.target = target or screen
        self.speed = speed
        self.gap = gap

        self.is_vector_font = isinstance(font_face, vector_font)

        if self.is_vector_font and font_size is None:
            raise ValueError("scroll_text: vector fonts require a font_size")

        self.target.font = self.font_face

        self.tw, self.th = self.target.measure_text(text, font_size) if isinstance(self.font_face, vector_font) else self.target.measure_text(text)
        if self.is_vector_font:
            self.th = font_size

        self.scroll_distance = self.tw + (gap if isinstance(gap, int) else self.target.width)

        self.t_start = None

        offset_y = align if isinstance(align, int) else 0

        if align == "middle":
            offset_y = (self.target.height - self.th) // 2

        if align == "bottom":
            offset_y = self.target.height - self.th

        self.offset = vec2(0, offset_y)

    def render(self, offset=None):
        self.target.font = self.font_face

        offset = offset or vec2(0, 0)
        offset += vec2(self.offset.x, self.offset.y)

        if self.is_vector_font:
            self.target.text(self.text, offset, self.font_size)
        else:
            self.target.text(self.text, offset)

        if isinstance(self.gap, int):
            while offset.x + self.tw < self.target.width:
                offset.x += self.tw + self.gap
                if self.is_vector_font:
                    self.target.text(self.text, offset, self.font_size)
                else:
                    self.target.text(self.text, offset)

    def update(self, progress=None):
        self.t_start = self.t_start if self.t_start is not None else time.ticks_ms()

        if progress is not None:
            timedelta = self.scroll_distance * progress
        else:
            timedelta = time.ticks_diff(time.ticks_ms(), self.t_start)
            timedelta /= 1000 / self.speed

            progress = timedelta / self.scroll_distance

        distance = timedelta % self.scroll_distance

        if isinstance(self.gap, int):
            self.offset.x = -distance
        else:
            self.offset.x = self.target.width - distance

        return progress

    def reset(self):
        self.t_start = None

    @property
    def scroll_time(self):
        return self.scroll_distance / self.speed

    def __call__(self):
        self.render()
        return self.update()


if __name__ == "__main__":
    import picovector # noqa F401

    test1 = Scroll("Hello World", gap=10, speed=10)
    test2 = Scroll("Hello World", gap=10, speed=10)
    total_scroll_time = test1.scroll_time
    print(total_scroll_time)

    frame = 0
    while True:
        screen.pen = color.black
        screen.clear()
        progress = test1.update(progress=frame / 67)  # scroll based on frame counter
        test2.update()  # scroll based on wall time
        screen.pen = color.white
        test1.render()
        screen.pen = color.white
        test2.render(offset=vec2(0, 10))
        print(progress, time.ticks_diff(time.ticks_ms(), test1.t_start))
        display.update()
        time.sleep(1.0 / 10)
        frame += 1
