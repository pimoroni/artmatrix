# Hardware error codes:
#
# E1  - PSRAM missing, or the wrong filesystem image is flashed.
# E2  - VLED rail out of range (supply, load switch or sense divider).
# E3  - Calibration did not finish.
# E4  - Panel half A drew no current.
# E5  - Panel half B drew no current.
# E6  - Panel halves drew badly mismatched current.
# E7  - Red plane drew no current.
# E8  - Green plane drew no current.
# E9  - Blue plane drew no current.
# E10 - SD card mount, write or read-back failed.
# E11 - Failed to start CYW43 (wireless module).
# E12 - Wireless scan fail - unable to detect networks.
# E13 - MAC address invalid.
# E14 - Time out during button test.
# E15 - Test passed but the flag would not clear.

import gc
import os
import time

import machine
import network
import sdcard
from machine import Pin
from pimoroni import Button

FLAG = "hardware_test.txt"

WIDTH, HEIGHT = screen.width, screen.height

WHITE = color.rgb(255, 255, 255)
BLACK = color.rgb(0, 0, 0)
RED = color.rgb(255, 0, 0)
GREEN = color.rgb(0, 255, 0)
BLUE = color.rgb(0, 0, 255)
DARK_BLUE = color.rgb(0, 40, 90)
DARK_RED = color.rgb(150, 0, 0)
DARK_GREEN = color.rgb(0, 120, 0)

CUR_COUNTS_PER_AMP = 118.26
VLED_MV_PER_COUNT = 1.61

PSRAM_MIN_FREE = 4 * 1024 * 1024
FS_MIN_BYTES = 12 * 1024 * 1024

VLED_GOOD_COUNTS = 2668
VLED_MAX_COUNTS = 3300
VLED_LOADED_MIN_COUNTS = 2606

CEILING_MIN = 0.05
CALIBRATE_START_TIMEOUT_MS = 2000
CALIBRATE_TIMEOUT_MS = 20000
CALIBRATE_SETTLE_MS = 1000

MEASURE_BRIGHTNESS = 0.5
MEASURE_SETTLE_MS = 500
MEASURE_SAMPLES = 8

IDLE_MAX_AMPS = 0.05
HALF_MIN_AMPS = 0.40
HALF_MAX_AMPS = 1.30
HALF_MATCH = 0.15

PLANE_LIMITS = (
    ("red", RED, "E7", 0.45, 1.60),
    ("green", GREEN, "E8", 0.18, 0.50),
    ("blue", BLUE, "E9", 0.15, 0.50),
)

ERROR_SIZE = 4
HALF_ERROR_SIZE = 2

OPERATOR_BRIGHTNESS = 0.5
BUTTON_TIMEOUT_MS = 60000
PASS_DWELL_MS = 2000

VENDOR = "28:CD:C1"

PATTERN_PHASES = (
    ("red", 1500),
    ("green", 1500),
    ("blue", 1500),
    ("white", 1500),
)

LED_WALK_MS = 400
LED_COLOURS = (RED, GREEN, BLUE)

BOX = 26
BOX_GAP = 4
BOX_Y = 76


class Tests:
    def __init__(self):
        screen.antialias = image.OFF
        screen.font = rom_font.nope
        screen.pen = WHITE

        self.wlan = None
        self.mac = None
        self.buttons = None
        self.led = 0
        self.led_at = 0

        self.readings = {}

    def centred(self, string, size, bounds=None):
        screen.text(string, bounds or rect(0, 0, WIDTH, HEIGHT), size,
                    align=(image.CENTER, image.MIDDLE))

    def message(self, lines, pen=DARK_BLUE):
        screen.pen = pen
        screen.clear()
        screen.pen = WHITE
        self.centred("\n".join(lines), 1)
        display.update()

    def fill(self, pen, x=0, w=WIDTH):
        screen.pen = BLACK
        screen.clear()
        screen.pen = pen
        screen.rectangle(x, 0, w, HEIGHT)
        display.update()

    def current_amps(self):
        time.sleep_ms(MEASURE_SETTLE_MS)
        total = 0
        for _ in range(MEASURE_SAMPLES):
            counts = display.power_stats()
            total += counts[3] - counts[5]
            time.sleep_ms(20)
        return (total / MEASURE_SAMPLES) / CUR_COUNTS_PER_AMP

    def measure(self, name, pen, x=0, w=WIDTH):
        self.fill(pen, x, w)
        amps = self.current_amps()
        self.readings[name] = amps
        counts = display.power_stats()
        print(f"{name}: {amps:.3f}A LED (raw {counts[3]} idle {counts[5]} counts, "
              f"brightness applied {counts[11]:.3f})")
        return amps

    def test_identity(self):
        print(os.uname())
        print(f"UID: {UID}")

        gc.collect()
        free = gc.mem_free()
        print(f"heap free: {free}")
        if free < PSRAM_MIN_FREE:
            raise Exception("E1")

        stat = os.statvfs("/")
        total = stat[1] * stat[2]
        print(f"filesystem: {total} bytes")
        if total < FS_MIN_BYTES:
            raise Exception("E1")

    def test_vled(self):
        counts = display.power_stats()
        vled_open, vled_mean, vled_min = counts[0], counts[1], counts[2]
        print(f"VLED open {vled_open} mean {vled_mean} min {vled_min} "
              f"({vled_open * VLED_MV_PER_COUNT / 1000:.2f}V open)")

        if vled_open < VLED_GOOD_COUNTS or vled_open > VLED_MAX_COUNTS:
            raise Exception("E2")

        if vled_min < VLED_LOADED_MIN_COUNTS:
            raise Exception("E2")

    def test_calibration(self):
        self.message(("Calibrating", "please wait"))
        time.sleep_ms(200)

        display.calibrate()

        start = time.ticks_ms()
        while not display.power_stats()[6]:
            if time.ticks_diff(time.ticks_ms(), start) > CALIBRATE_START_TIMEOUT_MS:
                raise Exception("E3")
            time.sleep_ms(10)

        start = time.ticks_ms()
        while display.power_stats()[6]:
            if time.ticks_diff(time.ticks_ms(), start) > CALIBRATE_TIMEOUT_MS:
                raise Exception("E3")
            time.sleep_ms(50)

        self.fill(BLACK)
        time.sleep_ms(CALIBRATE_SETTLE_MS)

        ceiling = display.power_stats()[9]
        print(f"brightness ceiling: {ceiling:.3f}")
        if ceiling < CEILING_MIN:
            raise Exception("E3")

    def test_panel_current(self):
        display.set_brightness(MEASURE_BRIGHTNESS)

        if self.measure("dark", BLACK) > IDLE_MAX_AMPS:
            raise Exception("E2")

        half = WIDTH // 2
        half_a = self.measure("half A", WHITE, 0, half)
        half_b = self.measure("half B", WHITE, half, half)

        if half_a < HALF_MIN_AMPS or half_a > HALF_MAX_AMPS:
            raise Exception("E4")
        if half_b < HALF_MIN_AMPS or half_b > HALF_MAX_AMPS:
            raise Exception("E5")

        larger = max(half_a, half_b)
        if abs(half_a - half_b) > larger * HALF_MATCH:
            raise Exception("E6")

        for name, pen, code, low, high in PLANE_LIMITS:
            amps = self.measure(name, pen)
            if amps < low or amps > high:
                raise Exception(code)

        self.fill(BLACK)

    def test_sd(self):
        path = "/sd/hardware_test.tmp"
        payload = f"artmatrix {UID}".encode()
        mounted = False

        try:
            spi = machine.SPI(0, baudrate=1_000_000, polarity=0, phase=0,
                              sck=Pin.board.SD_SCLK, mosi=Pin.board.SD_CMD, miso=Pin.board.SD_SD0)
            sd = sdcard.SDCard(spi, Pin(Pin.board.SD_CS, Pin.OUT, value=1))
            os.mount(sd, "/sd")
            mounted = True

            with open(path, "wb") as f:
                f.write(payload)

            with open(path, "rb") as f:
                if f.read() != payload:
                    raise Exception("E10")

            os.remove(path)
            print("SD: OK!")
        except OSError as e:
            print("SD:", e)
            raise Exception("E10") from None
        finally:
            if mounted:
                try:
                    os.umount("/sd")
                except OSError:
                    pass

    def test_wireless(self):
        try:
            self.wlan = network.WLAN(network.WLAN.IF_STA)
            self.wlan.active(True)
        except OSError:
            raise Exception("E11") from None

        self.mac = self.wlan.config("mac")
        address = ":".join([f"{b:02X}" for b in self.mac])
        print(f"MAC: {address}")

        try:
            results = self.wlan.scan()
        except OSError:
            raise Exception("E11") from None

        if not results:
            raise Exception("E12")

        print(f"scan: {len(results)} networks, strongest {max(r[3] for r in results)}dBm")

        if address[:8] != VENDOR:
            raise Exception("E13")

    def draw_pattern(self, phase):
        if phase == "red":
            self.fill(RED)
        elif phase == "green":
            self.fill(GREEN)
        elif phase == "blue":
            self.fill(BLUE)
        elif phase == "white":
            self.fill(WHITE)

    def draw_prompt(self):
        screen.pen = DARK_BLUE
        screen.clear()

        screen.pen = WHITE
        self.centred("Press\nA B C USER", 1, rect(0, 24, WIDTH, 30))

        names = ("A", "B", "C", "U")
        row = len(names) * BOX + (len(names) - 1) * BOX_GAP
        x = (WIDTH - row) // 2

        for name in names:
            screen.pen = DARK_GREEN if self.buttons[name][1] else BLACK
            screen.rectangle(x, BOX_Y, BOX, BOX)

            screen.pen = WHITE
            self.centred(name, 2, rect(x, BOX_Y, BOX, BOX))
            x += BOX + BOX_GAP

        display.update()

    def poll_buttons(self):
        changed = False
        for name in self.buttons:
            if self.buttons[name][0].raw() and not self.buttons[name][1]:
                self.buttons[name][1] = True
                changed = True
        return changed

    def walk_leds(self):
        if time.ticks_diff(time.ticks_ms(), self.led_at) > LED_WALK_MS:
            self.led_at = time.ticks_ms()
            self.led = (self.led + 1) % (4 * len(LED_COLOURS))
            lit = self.led % 4
            pen = LED_COLOURS[self.led // 4]
            leds(*[pen if i == lit else BLACK for i in range(4)])

    def show_patterns(self):
        for phase, duration in PATTERN_PHASES:
            self.draw_pattern(phase)
            start = time.ticks_ms()
            while time.ticks_diff(time.ticks_ms(), start) < duration:
                self.poll_buttons()
                self.walk_leds()
                time.sleep_ms(10)

    def test_operator(self):
        self.buttons = {
            "A": [Button(BUTTON_A), False],
            "B": [Button(BUTTON_B), False],
            "C": [Button(BUTTON_C), False],
            "U": [Button(BUTTON_USER), False],
        }

        display.set_brightness(OPERATOR_BRIGHTNESS)
        self.led_at = time.ticks_ms()
        self.led = 0

        self.show_patterns()

        self.draw_prompt()
        start = time.ticks_ms()

        while not all(state for _button, state in self.buttons.values()):
            if time.ticks_diff(time.ticks_ms(), start) > BUTTON_TIMEOUT_MS:
                raise Exception("E14")

            if self.poll_buttons():
                self.draw_prompt()

            self.walk_leds()
            time.sleep_ms(10)

        self.draw_prompt()
        leds.clear()
        print("BUTTONS: OK!")

    def clear_flag(self):
        if not file_exists(FLAG):
            return False

        try:
            os.remove(FLAG)
        except OSError:
            raise Exception("E15") from None

        if file_exists(FLAG):
            raise Exception("E15")

        return True

    def display_error(self, error):
        code = str(error)
        screen.pen = DARK_RED
        screen.clear()
        screen.pen = WHITE

        half = WIDTH // 2
        if code == "E4":
            self.centred(code, HALF_ERROR_SIZE, rect(half, 0, half, HEIGHT))
        elif code == "E5":
            self.centred(code, HALF_ERROR_SIZE, rect(0, 0, half, HEIGHT))
        else:
            self.centred(code, ERROR_SIZE)

        display.update()

        try:
            count = int(code[1:])
        except ValueError:
            count = 0

        for _ in range(count):
            leds(RED)
            time.sleep_ms(250)
            leds.clear()
            time.sleep_ms(250)
        time.sleep(2)

    def show_pass(self):
        screen.pen = DARK_GREEN
        screen.clear()
        screen.pen = WHITE
        self.centred("PASS", 3)
        display.update()
        leds(GREEN)

    def run(self):
        try:
            self.test_identity()
            self.test_vled()
            self.test_calibration()
            self.test_panel_current()
            self.test_sd()
            self.test_wireless()
            self.test_operator()

            print("PASS", UID, self.readings)
            self.show_pass()
            time.sleep_ms(PASS_DWELL_MS)

            if self.clear_flag():
                machine.reset()

            while True:
                time.sleep(1)

        except Exception as e:  # noqa: BLE001
            print(e)
            leds.clear()
            while True:
                self.display_error(e)


t = Tests()
t.run()
