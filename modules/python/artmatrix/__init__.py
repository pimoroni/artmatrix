import machine
import binascii
import builtins
import board

import _artmatrix
import picovector
from plasma import WS2812


builtins.BUTTON_A = board.SW_A
builtins.BUTTON_B = board.SW_B
builtins.BUTTON_C = board.SW_C
builtins.BUTTON_USER = board.SW_USER

builtins.UID = binascii.hexlify(machine.unique_id()).decode("ASCII")

for k, v in picovector.__dict__.items():
    if not k.startswith("__"):
        setattr(builtins, k, v)

# Hoist image anti-aliasing constants
builtins.OFF = image.OFF
builtins.X2 = image.X2
builtins.X4 = image.X4

builtins.display = _artmatrix.artmatrix()
builtins.screen = image(128, 128, memoryview(display))

builtins.sensor = machine.ADC(board.SENSOR_IN)


# LED control
class LEDS:
    def __init__(self):
        self._leds = WS2812(4)
        self._leds.start()

    def _to_rgb(self, i):
        if isinstance(i, color):
            i = i.p & 0xffffffff
            return (i >> 0) & 0xff, (i >> 8) & 0xff, (i >> 16) & 0xff
        if isinstance(i, tuple):
            return i
        return i, i, i

    def __call__(self, *args):
        args = (args[0], ) * 4 if len(args) == 1 else args
        for i in range(4):
            self._leds.set_rgb(3 - i, *self._to_rgb(args[i]))

    def clear(self):
        self(0)


builtins.leds = LEDS()

# Import modules
__import__(".frozen/artmatrix/math")
__import__(".frozen/artmatrix/text")
__import__(".frozen/artmatrix/sprite")
__import__(".frozen/artmatrix/filesystem")
__import__(".frozen/artmatrix/memory")
# screen.antialias = image.X4
# screen.font = rom_font.more
# screen.pen = color.black
# screen.clear()
# screen.pen = color.white
# screen.text("Ready!", 28, 50)
# display.update()
