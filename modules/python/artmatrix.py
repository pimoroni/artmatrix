import _artmatrix
import machine
import board
import neopixel
import builtins
import picovector


builtins.BUTTON_A = board.SW_A
builtins.BUTTON_B = board.SW_B
builtins.BUTTON_C = board.SW_C
builtins.BUTTON_USER = board.SW_USER


for k, v in picovector.__dict__.items():
    if not k.startswith("__"):
        setattr(builtins, k, v)

builtins.display = _artmatrix.artmatrix()
builtins.screen = image(128, 128, memoryview(display))

builtins.sensor = machine.ADC(board.SENSOR_IN)

_led = neopixel.NeoPixel(board.LEDS, 1)


def led(*args):
    if len(args) == 3:
        _led[0] = args
        _led.write()
    return _led[0]


builtins.led = led
