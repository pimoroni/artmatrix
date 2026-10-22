import machine
import binascii
from array import array
import builtins
import board

import _artmatrix
import picovector


builtins.BUTTON_A = board.SW_A
builtins.BUTTON_B = board.SW_B
builtins.BUTTON_C = board.SW_C
builtins.BUTTON_USER = board.SW_USER

builtins.UID = binascii.hexlify(machine.unique_id()).decode("ascii")

for k, v in picovector.__dict__.items():
    if not k.startswith("__"):
        setattr(builtins, k, v)

# Hoist image anti-aliasing constants
builtins.OFF = image.OFF
builtins.X2 = image.X2
builtins.X4 = image.X4

builtins.display = _artmatrix.artmatrix()
builtins.screen = image(128, 128, memoryview(display))


# The display driver owns the ADC so it can watch VLED_SENSE and CUR_SENSE
# without racing us over adc_select_input. It samples the SENS header for us,
# as a rolling 4ms average rather than an on-demand conversion.
class _Sensor:
    def read_u16(self):
        return display.sensor()


builtins.sensor = _Sensor()


# A mic on the SENS header, analysed in C: display.hear() works through every 4ms
# capture since the last call, so calling several of these in a frame agrees.
# Analyses whatever is on SENS; there's no telling if it's a mic.
class _Mic:
    def __init__(self):
        # level, tone, flux, intensity, active, bpm, phase, then a count and a
        # strength for each of beat, kick and clap, 16 bands and 64 wave points
        self._out = array("f", bytes(93 * 4))
        self._bands = memoryview(self._out)[13:29]
        self._wave = memoryview(self._out)[29:93]
        self._seen = [0, 0, 0]

    # 0, or once per hit since the last call its strength 0-1, never 0 for a real one
    def _hit(self, slot):
        display.hear(self._out)
        count = self._out[slot]
        which = (slot - 7) // 2
        if count == self._seen[which]:
            return 0
        self._seen[which] = count
        return self._out[slot + 1]

    def level(self):
        display.hear(self._out)
        return self._out[0]

    def tone(self):
        display.hear(self._out)
        return self._out[1]

    def flux(self):
        display.hear(self._out)
        return self._out[2]

    def intensity(self):
        display.hear(self._out)
        return self._out[3]

    def active(self):
        display.hear(self._out)
        return self._out[4] > 0

    def bpm(self):
        display.hear(self._out)
        return self._out[5]

    def phase(self):
        display.hear(self._out)
        return self._out[6]

    def beat(self):
        return self._hit(7)

    def kick(self):
        return self._hit(9)

    def clap(self):
        return self._hit(11)

    # 16 bands low to high, 0-1. the same view every call, so copy it to keep it
    def spectrum(self):
        display.hear(self._out)
        return self._bands

    # the newest capture as 64 points, -1 to 1. the same view every call
    def waveform(self):
        display.hear(self._out)
        return self._wave


builtins.mic = _Mic()


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
