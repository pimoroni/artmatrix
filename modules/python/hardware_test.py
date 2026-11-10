# hardware error codes:
#
# E1  - PSRAM missing, or the wrong filesystem image is flashed.
# E2  - VLED rail out of range (supply, load switch or sense divider).
# E3  - calibration didn't finish, or the supply is too weak.
#
# E4  - panel half A drew no current.
# E5  - panel half B drew no current.
# E6  - panel halves drew badly mismatched current.
#
# E7  - red plane drew no current.
# E8  - green plane drew no current.
# E9  - blue plane drew no current.
#
# E10 - SD card mount, write or read-back failed.
# E11 - failed to start CYW43 (wireless module).
# E12 - wireless scan failed, no networks found.
# E13 - MAC address invalid.
#
# E14 - timed out during the button test.
# E15 - test passed but the flag wouldn't clear.
# E16 - panel data lines shorted together.

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

STAT_CURRENT = 3
STAT_IDLE = 5
STAT_CALIBRATING = 6
STAT_CEILING = 9
STAT_APPLIED = 11

PSRAM_MIN_FREE = 4 * 1024 * 1024
FS_MIN_BYTES = 12 * 1024 * 1024

VLED_GOOD_COUNTS = 2668
VLED_MAX_COUNTS = 3300
VLED_LOADED_MIN_COUNTS = 2606

CEILING_MIN = 0.25
MESSAGE_SHOW_MS = 200
CALIBRATE_START_TIMEOUT_MS = 2000
CALIBRATE_START_POLL_MS = 10
CALIBRATE_TIMEOUT_MS = 20000
CALIBRATE_POLL_MS = 50
CALIBRATE_SETTLE_MS = 1000

MEASURE_BRIGHTNESS = 0.5
MEASURE_SETTLE_MS = 500
MEASURE_SAMPLES = 8
MEASURE_SAMPLE_GAP_MS = 20

IDLE_MAX_AMPS = 0.05
HALF_MIN_AMPS = 0.40
HALF_MAX_AMPS = 1.30
HALF_MATCH = 0.15
QUARTER_MATCH = 0.25

FULL_SCREEN = ((0, WIDTH),)
HALF_A = ((0, WIDTH // 2),)
HALF_B = ((WIDTH // 2, WIDTH // 2),)
FIRST_QUARTERS = ((0, WIDTH // 4), (WIDTH // 2, WIDTH // 4))

PLANE_LIMITS = (
    ("red", RED, "E7", 0.45, 1.60),
    ("green", GREEN, "E8", 0.18, 0.50),
    ("blue", BLUE, "E9", 0.15, 0.50),
)

TEXT_SIZE = 1
LABEL_SIZE = 2
PASS_SIZE = 3
ERROR_SIZE = 4
HALF_ERROR_SIZE = 2

ERROR_BLINK_MS = 250
ERROR_PAUSE_MS = 2000

OPERATOR_BRIGHTNESS = 0.5
BUTTON_TIMEOUT_MS = 60000
BUTTON_POLL_MS = 10
PASS_DWELL_MS = 2000

VENDOR = "28:CD:C1"
SCAN_RSSI = 3

PATTERN_MS = 1500

LED_CYCLE_MS = 400
LED_COLOURS = (RED, GREEN, BLUE)
LED_STRIP_ROWS = 32

PROMPT_Y = 36
PROMPT_HEIGHT = 30

BOX = 26
BOX_GAP = 4
BOX_Y = 76


class Tests:
    def __init__(self):
        screen.antialias = image.OFF
        screen.font = rom_font.nope
        screen.pen = WHITE

        self.buttons = None
        self.pressed = set()
        self.led = 0
        self.led_at = 0

        self.readings = {}

    def centred(self, string, size, bounds=None):
        screen.text(string, bounds or rect(0, 0, WIDTH, HEIGHT), size,
                    align=(image.CENTER, image.MIDDLE))

    def message(self, lines):
        screen.pen = DARK_BLUE
        screen.clear()
        screen.pen = WHITE
        self.centred("\n".join(lines), TEXT_SIZE)
        display.update()

    def fill(self, pen, spans=FULL_SCREEN):
        screen.pen = BLACK
        screen.clear()
        screen.pen = pen
        for x, width in spans:
            screen.rectangle(x, 0, width, HEIGHT)
        display.update()

    def current_amps(self):
        time.sleep_ms(MEASURE_SETTLE_MS)
        total = 0
        for _ in range(MEASURE_SAMPLES):
            counts = display.power_stats()
            total += counts[STAT_CURRENT] - counts[STAT_IDLE]
            time.sleep_ms(MEASURE_SAMPLE_GAP_MS)
        return (total / MEASURE_SAMPLES) / CUR_COUNTS_PER_AMP

    def measure(self, name, pen, spans=FULL_SCREEN, dwell=0):
        start = time.ticks_ms()
        self.fill(pen, spans)
        amps = self.current_amps()
        self.readings[name] = amps
        counts = display.power_stats()
        print(f"{name}: {amps:.3f}A LED (raw {counts[STAT_CURRENT]} idle {counts[STAT_IDLE]} counts, "
              f"brightness applied {counts[STAT_APPLIED]:.3f})")
        elapsed = time.ticks_diff(time.ticks_ms(), start)
        time.sleep_ms(max(0, dwell - elapsed))
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
        block_size, block_count = stat[1], stat[2]
        total = block_size * block_count
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
        time.sleep_ms(MESSAGE_SHOW_MS)

        display.calibrate()

        start = time.ticks_ms()
        while not display.power_stats()[STAT_CALIBRATING]:
            if time.ticks_diff(time.ticks_ms(), start) > CALIBRATE_START_TIMEOUT_MS:
                raise Exception("E3")
            time.sleep_ms(CALIBRATE_START_POLL_MS)

        start = time.ticks_ms()
        while display.power_stats()[STAT_CALIBRATING]:
            if time.ticks_diff(time.ticks_ms(), start) > CALIBRATE_TIMEOUT_MS:
                raise Exception("E3")
            time.sleep_ms(CALIBRATE_POLL_MS)

        self.fill(BLACK)
        time.sleep_ms(CALIBRATE_SETTLE_MS)

        ceiling = display.power_stats()[STAT_CEILING]
        print(f"brightness ceiling: {ceiling:.3f}")
        if ceiling < CEILING_MIN:
            raise Exception("E3")

    def test_panel_current(self):
        display.set_brightness(MEASURE_BRIGHTNESS)

        if self.measure("dark", BLACK) > IDLE_MAX_AMPS:
            raise Exception("E2")

        half_a = self.measure("half A", WHITE, HALF_A)
        half_b = self.measure("half B", WHITE, HALF_B)

        if half_a < HALF_MIN_AMPS or half_a > HALF_MAX_AMPS:
            raise Exception("E4")
        if half_b < HALF_MIN_AMPS or half_b > HALF_MAX_AMPS:
            raise Exception("E5")

        larger = max(half_a, half_b)
        if abs(half_a - half_b) > larger * HALF_MATCH:
            raise Exception("E6")

        for name, pen, code, low, high in PLANE_LIMITS:
            amps = self.measure(name, pen, dwell=PATTERN_MS)
            if amps < low or amps > high:
                raise Exception(code)

        self.measure("white", WHITE, dwell=PATTERN_MS)

        for name, pen, _code, _low, _high in PLANE_LIMITS:
            expected = self.readings[name] / 2
            amps = self.measure(f"{name} first quarters", pen, FIRST_QUARTERS)
            if abs(amps - expected) > expected * QUARTER_MATCH:
                raise Exception("E16")

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

            with open(path, "wb") as file:
                file.write(payload)

            with open(path, "rb") as file:
                if file.read() != payload:
                    raise Exception("E10")

            os.remove(path)
            print("SD: OK!")
        except OSError as error:
            print("SD:", error)
            raise Exception("E10") from None
        finally:
            if mounted:
                try:
                    os.umount("/sd")
                except OSError:
                    pass

    def test_wireless(self):
        try:
            wlan = network.WLAN(network.WLAN.IF_STA)
            wlan.active(True)
        except OSError:
            raise Exception("E11") from None

        mac = wlan.config("mac")
        address = ":".join([f"{byte:02X}" for byte in mac])
        print(f"MAC: {address}")

        try:
            results = wlan.scan()
        except OSError:
            raise Exception("E11") from None

        if not results:
            raise Exception("E12")

        strongest = max(result[SCAN_RSSI] for result in results)
        print(f"scan: {len(results)} networks, strongest {strongest}dBm")

        if not address.startswith(VENDOR):
            raise Exception("E13")

    def draw_prompt(self):
        screen.pen = DARK_BLUE
        screen.clear()

        screen.pen = WHITE
        self.centred("Press\nA B C USER", TEXT_SIZE, rect(0, PROMPT_Y, WIDTH, PROMPT_HEIGHT))

        names = ("A", "B", "C", "U")
        row = len(names) * BOX + (len(names) - 1) * BOX_GAP
        x = (WIDTH - row) // 2

        for name in names:
            screen.pen = DARK_GREEN if name in self.pressed else BLACK
            screen.rectangle(x, BOX_Y, BOX, BOX)

            screen.pen = WHITE
            self.centred(name, LABEL_SIZE, rect(x, BOX_Y, BOX, BOX))
            x += BOX + BOX_GAP

        display.update()

    def poll_buttons(self):
        changed = False
        for name, button in self.buttons.items():
            if button.raw() and name not in self.pressed:
                self.pressed.add(name)
                changed = True
        return changed

    def cycle_leds(self, redraw=False):
        due = time.ticks_diff(time.ticks_ms(), self.led_at) > LED_CYCLE_MS
        if due:
            self.led_at = time.ticks_ms()
            self.led = (self.led + 1) % len(LED_COLOURS)
        if due or redraw:
            screen.pen = LED_COLOURS[self.led]
            screen.rectangle(0, 0, WIDTH, LED_STRIP_ROWS)
            display.update()

    def test_operator(self):
        self.buttons = {
            "A": Button(BUTTON_A),
            "B": Button(BUTTON_B),
            "C": Button(BUTTON_C),
            "U": Button(BUTTON_USER),
        }

        display.set_brightness(OPERATOR_BRIGHTNESS)
        self.led_at = time.ticks_ms()
        self.led = 0

        self.draw_prompt()
        self.cycle_leds(True)
        start = time.ticks_ms()

        while len(self.pressed) < len(self.buttons):
            if time.ticks_diff(time.ticks_ms(), start) > BUTTON_TIMEOUT_MS:
                raise Exception("E14")

            if self.poll_buttons():
                self.draw_prompt()
                self.cycle_leds(True)
            else:
                self.cycle_leds()
            time.sleep_ms(BUTTON_POLL_MS)

        self.draw_prompt()
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
        if not (code.startswith("E") and code[1:].isdigit()):
            code = "ERR"
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

        count = 0 if code == "ERR" else int(code[1:])

        for _ in range(count):
            display.set_ambient(True)
            time.sleep_ms(ERROR_BLINK_MS)
            display.set_ambient(False)
            time.sleep_ms(ERROR_BLINK_MS)
        display.set_ambient(True)
        time.sleep_ms(ERROR_PAUSE_MS)

    def show_pass(self):
        screen.pen = DARK_GREEN
        screen.clear()
        screen.pen = WHITE
        self.centred("PASS", PASS_SIZE)
        display.update()

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

        except Exception as error:  # noqa: BLE001
            print(error)
            while True:
                self.display_error(error)


tests = Tests()
tests.run()
