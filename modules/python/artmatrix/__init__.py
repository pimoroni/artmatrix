import board
import neopixel

_led = neopixel.NeoPixel(board.LEDS, 1)
_led_values = (0, 0, 0)


def led(*args):
    global _led_values
    if args:
        _led_values = (args[0], ) * 3 if len(args) == 1 else args
        _led[0] = [int(v ** 2.2 * 255) for v in _led_values]
        _led.write()
    return _led_values


led(0.1, 0, 0)

import _artmatrix
import machine

import binascii
import builtins
import picovector


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

builtins.led = led

# Import modules
__import__(".frozen/artmatrix/math")
__import__(".frozen/artmatrix/text")
__import__(".frozen/artmatrix/sprite")
__import__(".frozen/artmatrix/filesystem")
__import__(".frozen/artmatrix/memory")

screen.antialias = image.X4
screen.font = rom_font.more
screen.pen = color.black
screen.clear()
screen.pen = color.white
screen.text("Ready!", 28, 50)
display.update()

led(0)
