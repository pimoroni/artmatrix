import time
import re
import machine  # noqa: F401
import micropython
from micropython import const
from random import randint

"""
Conway's Game Of Life for Interstate 75
You can load game of life RLE files below to try different patterns
See https://conwaylife.com/ref/DRH/ to find some RLE examples.
"""

# Runs ~20fps with stock clock,

# uncomment the below for ~30fps
# machine.freq(200_000_000)

# uncomment the below for ~40fps
# machine.freq(250_000_000)

# Vibrant yellow, trailing off to purple
PLASMA = b'\x00\x00\x00\x00\x00\x00\x00\x00\x03\x00\x11\x00\x07\x01$\x00\x0c\x027\x00\x13\x02K\x00\x15\x02L\x00\x17\x02M\x00\x19\x02N\x00\x1b\x02P\x00\x1d\x02Q\x00\x1f\x02R\x00!\x01S\x00#\x01T\x00%\x01V\x00\'\x01W\x00)\x01X\x00+\x01Y\x00-\x00Z\x00/\x00[\x001\x00\\\x004\x00]\x006\x00^\x008\x00^\x00:\x00_\x00<\x00`\x00>\x00a\x00@\x00a\x00B\x00b\x00D\x00b\x00G\x00c\x00I\x01c\x00K\x01d\x00M\x02d\x00O\x03d\x00Q\x04d\x00T\x05d\x00V\x06d\x00X\x08d\x00Z\td\x00\\\nd\x00_\x0cd\x00a\rd\x00c\x0fc\x00e\x10c\x00g\x12b\x00j\x13b\x00l\x15a\x00n\x17a\x00p\x18`\x00r\x1a_\x00t\x1b_\x00w\x1d^\x00y\x1f]\x00{ ]\x00}"\\\x00\x7f$[\x00\x81&Z\x00\x83\'Y\x00\x86)Y\x00\x88+X\x00\x8a-W\x00\x8c/V\x00\x8e0U\x00\x913T\x00\x935S\x00\x957R\x00\x979Q\x00\x99;Q\x00\x9b=P\x00\x9d?O\x00\x9fAN\x00\xa2CM\x00\xa4EL\x00\xa6GK\x00\xa8IJ\x00\xaaKJ\x00\xacNI\x00\xaePH\x00\xb0RG\x00\xb2TF\x00\xb4WE\x00\xb6YD\x00\xb8\\C\x00\xba^B\x00\xbcaA\x00\xbec@\x00\xc0f?\x00\xc2h>\x00\xc4k=\x00\xc6n<\x00\xc8p;\x00\xcas:\x00\xccv8\x00\xcey7\x00\xd0|6\x00\xd2\x7f5\x00\xd3\x824\x00\xd5\x853\x00\xd7\x881\x00\xd9\x8b0\x00\xda\x8f/\x00\xdc\x92.\x00\xdd\x95-\x00\xdf\x99+\x00\xe0\x9c*\x00\xe2\xa0)\x00\xe3\xa3(\x00\xe4\xa7\'\x00\xe5\xab&\x00\xe7\xaf%\x00\xe8\xb2$\x00\xe9\xb6#\x00\xea\xba#\x00\xeb\xbe"\x00\xeb\xc2"\x00\xec\xc7"\x00\xed\xcb"\x00\xed\xcf"\x00\xee\xd3#\x00\xee\xd8#\x00\xef\xdc$\x00\xef\xe1%\x00\xef\xe5%\x00\xef\xea&\x00\xef\xef&\x00\xef\xf3%\x00\xef\xf8!\x00'

# Neutral toned with a trail off to blue
TWILIGHT = b'\x00\x00\x00\x00\x00\x00\x00\x00\x1c\x1b\x1c\x00778\x00RRT\x00lmp\x00jmp\x00hlo\x00ekn\x00cjn\x00`im\x00]hm\x00[gm\x00Xfl\x00Uel\x00Scl\x00Pbl\x00Nal\x00K_l\x00I^l\x00G\\m\x00EZm\x00DYm\x00BWm\x00@Un\x00?Sn\x00>Rn\x00=Pn\x00<Nn\x00;Lo\x00;Jo\x00:Go\x00:Eo\x00:Co\x00:@n\x00:>n\x00:<n\x00:9m\x00:6l\x00:4l\x00:1k\x00:.j\x00;+h\x00;)g\x00;&e\x00;#c\x00: a\x00:\x1d_\x00:\x1a\\\x009\x18Y\x008\x15U\x007\x13R\x005\x11N\x004\x10J\x002\x0eE\x000\rA\x00.\x0c=\x00,\x0c9\x00*\x0b5\x00(\x0b2\x00&\x0b/\x00%\x0b,\x00#\x0c*\x00"\r(\x00"\r&\x00$\x0c\'\x00&\x0c(\x00(\x0c)\x00+\x0c*\x00.\x0c,\x002\r.\x005\r0\x009\x0e1\x00=\x0f3\x00B\x105\x00F\x117\x00J\x128\x00O\x13:\x00S\x14;\x00X\x16<\x00\\\x18=\x00a\x1a>\x00e\x1c>\x00i\x1e?\x00m ?\x00q#?\x00u&?\x00y)@\x00}-@\x00\x810@\x00\x844A\x00\x887A\x00\x8b;B\x00\x8e?B\x00\x91CC\x00\x95GD\x00\x98KF\x00\x9aPG\x00\x9dTI\x00\xa0YK\x00\xa3]M\x00\xa5bO\x00\xa8gR\x00\xaakU\x00\xacpY\x00\xaeu]\x00\xb0za\x00\xb2\x80f\x00\xb4\x85k\x00\xb6\x8ap\x00\xb8\x8fv\x00\xba\x94|\x00\xbc\x99\x82\x00\xbf\x9f\x89\x00\xc1\xa4\x90\x00\xc3\xa9\x97\x00\xc6\xae\x9e\x00\xc9\xb3\xa5\x00\xcb\xb8\xad\x00\xce\xbd\xb4\x00\xd1\xc2\xbc\x00\xd4\xc6\xc3\x00\xd7\xca\xc9\x00\xd9\xce\xcf\x00\xdb\xd2\xd5\x00\xde\xd4\xd9\x00\xe0\xd7\xde\x00\xe1\xd8\xe1\x00'

# Yellow/green with a trail off to blue
VIRIDIS = b'\x00\x00\x00\x00\x00\x00\x00\x00\x08\x00\x0b\x00\x11\x02\x17\x00\x1b\x04$\x00$\x082\x00%\t4\x00%\x0b6\x00%\x0c7\x00&\x0e9\x00&\x0f:\x00&\x11<\x00&\x12=\x00&\x14?\x00&\x15@\x00&\x17B\x00&\x18C\x00&\x1aD\x00&\x1bF\x00&\x1dG\x00&\x1eH\x00& I\x00&!J\x00&#K\x00%$L\x00%&M\x00%\'N\x00$)O\x00$*P\x00$,P\x00#-Q\x00#/R\x00"1S\x00"2S\x00"4T\x00!5U\x00!7U\x00 8V\x00 :W\x00 <W\x00\x1f=X\x00\x1f?X\x00\x1e@Y\x00\x1eBY\x00\x1eDZ\x00\x1dE[\x00\x1dG[\x00\x1dH\\\x00\x1cJ\\\x00\x1cL]\x00\x1bM]\x00\x1bO^\x00\x1bQ^\x00\x1aR_\x00\x1aT_\x00\x1aV`\x00\x19X`\x00\x19Ya\x00\x19[a\x00\x18]a\x00\x18_b\x00\x18ab\x00\x17bc\x00\x17dc\x00\x16gc\x00\x16ic\x00\x16kd\x00\x16md\x00\x16od\x00\x16qd\x00\x16sd\x00\x16ud\x00\x16wd\x00\x17yd\x00\x18{d\x00\x19}d\x00\x1a\x7fd\x00\x1b\x81c\x00\x1c\x83c\x00\x1e\x85c\x00 \x87b\x00!\x89b\x00$\x8ca\x00&\x8e`\x00(\x90`\x00+\x92_\x00.\x94^\x001\x96]\x004\x99\\\x007\x9b[\x00:\x9dZ\x00>\x9fX\x00A\xa1W\x00E\xa4V\x00I\xa6T\x00M\xa8S\x00Q\xaaQ\x00U\xacO\x00Y\xaeM\x00^\xb1K\x00b\xb3I\x00g\xb5G\x00l\xb7E\x00q\xb9C\x00v\xbb@\x00{\xbd>\x00\x80\xbf;\x00\x85\xc19\x00\x8b\xc36\x00\x90\xc53\x00\x96\xc70\x00\x9c\xc9-\x00\xa1\xcb*\x00\xa7\xcd(\x00\xad\xcf%\x00\xb3\xd1"\x00\xb9\xd2\x1f\x00\xbf\xd4\x1c\x00\xc5\xd6\x1a\x00\xcb\xd8\x19\x00\xd2\xda\x17\x00\xd8\xdb\x17\x00\xde\xdd\x17\x00\xe4\xdf\x19\x00\xea\xe1\x1b\x00\xf0\xe3\x1d\x00\xf7\xe5!\x00\xfd\xe7$\x00'

# Heap map style
TURBO = b'\x00\x00\x00\x00\x00\x00\x00\x00\x06\x03\x0b\x00\x0e\t\x19\x00\x16\x10,\x00\x1e\x18A\x00\x1f\x1bH\x00 \x1eN\x00!!T\x00"$Z\x00#\'`\x00#+e\x00$.j\x00%1o\x00%4t\x00&7x\x00&:|\x00&=\x80\x00\'A\x83\x00\'D\x86\x00\'G\x89\x00\'J\x8c\x00\'M\x8e\x00&Q\x90\x00%T\x91\x00$W\x92\x00#[\x92\x00!^\x92\x00\x1fb\x91\x00\x1de\x91\x00\x1ci\x8f\x00\x1al\x8e\x00\x18p\x8c\x00\x16s\x8a\x00\x14v\x88\x00\x12z\x86\x00\x11}\x83\x00\x10\x80\x80\x00\x0f\x83~\x00\x0e\x86{\x00\x0e\x89y\x00\x0f\x8cv\x00\x10\x8ft\x00\x11\x91r\x00\x13\x94o\x00\x16\x96l\x00\x19\x99i\x00\x1c\x9bf\x00 \x9db\x00$\x9f_\x00)\xa1[\x00.\xa3W\x003\xa5S\x009\xa7O\x00>\xa9J\x00D\xaaF\x00J\xacB\x00O\xad?\x00U\xaf;\x00[\xb07\x00`\xb14\x00f\xb11\x00k\xb2.\x00p\xb3,\x00v\xb2)\x00z\xb2\'\x00\x7f\xb2&\x00\x83\xb2&\x00\x88\xb1%\x00\x8c\xb1%\x00\x91\xb0&\x00\x95\xaf&\x00\x9a\xad&\x00\x9e\xac\'\x00\xa2\xaa(\x00\xa6\xa9)\x00\xaa\xa7)\x00\xae\xa5*\x00\xb2\xa3+\x00\xb6\xa1,\x00\xb9\x9e,\x00\xbc\x9c-\x00\xbf\x9a-\x00\xc2\x97-\x00\xc5\x95-\x00\xc7\x92,\x00\xc9\x8f+\x00\xcb\x8c*\x00\xcd\x88)\x00\xce\x85\'\x00\xcf\x81&\x00\xd0}$\x00\xd1y"\x00\xd2u \x00\xd3p\x1e\x00\xd3l\x1c\x00\xd3g\x1a\x00\xd3c\x18\x00\xd3^\x16\x00\xd2Z\x14\x00\xd2U\x12\x00\xd1Q\x10\x00\xd0L\x0e\x00\xcfH\r\x00\xceD\x0b\x00\xcc@\n\x00\xcb=\t\x00\xc99\x08\x00\xc76\x07\x00\xc53\x06\x00\xc30\x05\x00\xc0,\x04\x00\xbd)\x03\x00\xbb&\x03\x00\xb7#\x02\x00\xb4 \x02\x00\xb0\x1d\x01\x00\xad\x1b\x01\x00\xa9\x18\x01\x00\xa4\x15\x01\x00\xa0\x13\x01\x00\x9b\x10\x00\x00\x96\x0e\x01\x00\x91\x0c\x01\x00\x8c\n\x01\x00\x86\x07\x01\x00\x80\x05\x02\x00z\x04\x02\x00'

USE_PALETTE = PLASMA

SEED_AT = 1000  # Which generation to run to until re-seeding, 0 to never re-seed


# These need to be constants for the viper optimized block below.
WIDTH = const(128)
HEIGHT = const(128)


@micropython.viper
def compute_gol(board: ptr8, new_board: ptr8):  # noqa: F821
    for y in range(HEIGHT):
        alive = 0
        if y > 0:
            alive += board[(y - 1) * WIDTH] & 0x80
            alive += board[(y - 1) * WIDTH + 1] & 0x80

        alive += board[y * WIDTH + 1] & 0x80

        if y < HEIGHT - 1:
            alive += board[(y + 1) * WIDTH] & 0x80
            alive += board[(y + 1) * WIDTH + 1] & 0x80

        cur_val = board[y * WIDTH]
        if alive == 0x180:
            new_board[y * WIDTH] = 0x80
        elif alive == 0x100 and cur_val == 0x80:
            new_board[y * WIDTH] = cur_val
        elif cur_val > 0:
            new_board[y * WIDTH] = cur_val - 1
        else:
            new_board[y * WIDTH] = 0

        for x in range(1, WIDTH - 1):
            alive = 0
            if y > 0:
                alive += board[(y - 1) * WIDTH + x - 1] & 0x80
                alive += board[(y - 1) * WIDTH + x] & 0x80
                alive += board[(y - 1) * WIDTH + x + 1] & 0x80

            alive += board[y * WIDTH + x - 1] & 0x80
            alive += board[y * WIDTH + x + 1] & 0x80

            if y < HEIGHT - 1:
                alive += board[(y + 1) * WIDTH + x - 1] & 0x80
                alive += board[(y + 1) * WIDTH + x] & 0x80
                alive += board[(y + 1) * WIDTH + x + 1] & 0x80

            cur_val = board[y * WIDTH + x]
            if alive == 0x180:
                new_board[y * WIDTH + x] = 0x80
            elif alive == 0x100 and cur_val == 0x80:
                new_board[y * WIDTH + x] = cur_val
            elif cur_val > 0:
                new_board[y * WIDTH + x] = cur_val - 1
            else:
                new_board[y * WIDTH + x] = 0

        alive = 0
        if y > 0:
            alive += board[y * WIDTH - 2] & 0x80
            alive += board[y * WIDTH - 1] & 0x80
        alive += board[(y + 1) * WIDTH - 2] & 0x80

        if y < HEIGHT - 1:
            alive += board[(y + 2) * WIDTH - 2] & 0x80
            alive += board[(y + 2) * WIDTH - 1] & 0x80

        cur_val = board[(y + 1) * WIDTH - 1]
        if alive == 0x180:
            new_board[(y + 1) * WIDTH - 1] = 0x80
        elif alive == 0x100 and cur_val == 0x80:
            new_board[(y + 1) * WIDTH - 1] = cur_val
        elif cur_val > 0:
            new_board[(y + 1) * WIDTH - 1] = cur_val - 1
        else:
            new_board[(y + 1) * WIDTH - 1] = 0


@micropython.viper
def viper_display(source: ptr8, dest: ptr32, palette: ptr32):  # noqa: F821
    for offset in range(HEIGHT * WIDTH):
        # The framebuffer is in the format __RRGGBB, eg: 0x000000ff is blue and 0x00ff0000 is red
        # You can play with the values below to change the colour of the life animations.
        # We've created some presets for inspiration!

        # Palette Life
        dest[offset] = palette[source[offset]] if source[offset] < 0x80 else 0x00ffffff

        # Blue Life
        # dest[offset] = source[offset] if source[offset] < 0x80 else 0x009696ff

        # Green Life
        # dest[offset] = source[offset] << 8 if source[offset] < 0x80 else 0x0096ff96

        # Red Life
        # dest[offset] = source[offset] << 16 if source[offset] < 0x80 else 0x00ff9696

        # Purple Life
        # dest[offset] = (source[offset] << 16) | source[offset] if source[offset] < 0x80 else 0x00ff96ff

        # Yellow Life
        # dest[offset] = (source[offset] << 16) | (source[offset] << 8) if source[offset] < 0x80 else 0x00ffff96

        # Teal Life
        # dest[offset] = (source[offset] << 8) | source[offset] if source[offset] < 0x80 else 0x0096ffff


@micropython.viper
def make_display_ptr32(display) -> ptr32:  # noqa: F821
    return ptr32(memoryview(display))      # noqa: F821


class GameOfLife:
    def __init__(self, randomize=True, palette=PLASMA):
        self.palette = palette

        self.board = bytearray(WIDTH * HEIGHT)
        self.back_board = bytearray(WIDTH * HEIGHT)

        if randomize:
            self.seed_life()

    def seed_life(self):
        for i in range(WIDTH * HEIGHT):
            if randint(0, 3) == 3:
                self.board[i] = 0x80

    @micropython.native
    def compute(self):
        compute_gol(self.board, self.back_board)
        self.board, self.back_board = self.back_board, self.board

    def display(self, display):
        dest = make_display_ptr32(display)
        viper_display(self.board, dest, self.palette)

    def load_rle(self, filename):
        with open(filename, "r") as f:
            first_line = True
            pattern_width = 0
            pattern_height = 0
            finished = False

            while not finished:
                line = f.readline()
                if len(line) == 0:
                    break

                line = line.strip()
                if line[0] == "#":
                    continue

                if first_line:
                    params = line.split(",")
                    for param in params:
                        key, value = param.split("=")
                        key = key.strip()
                        value = value.strip()
                        if key == "x":
                            pattern_width = int(value)
                        elif key == "y":
                            pattern_height = int(value)
                        else:
                            print(f"Unknown header param: {key} = {value}")

                    first_line = False
                    y = (HEIGHT - pattern_height) // 2
                    x = (WIDTH - pattern_width) // 2
                    continue

                start = 0
                while start < len(line):
                    if line[start] == "!":
                        finished = True
                        break

                    m = re.match(r"([0-9]*)([bo$])", line[start:])
                    if m is not None:
                        repeat = 1
                        if len(m.group(1)) > 0:
                            repeat = int(m.group(1))

                        if m.group(2) == "$":
                            y += repeat
                            x = (WIDTH - pattern_width) // 2
                        elif m.group(2) == "o":
                            for _ in range(repeat):
                                self.board[y * WIDTH + x] = 0x80
                                x += 1
                        else:
                            x += repeat

                        start += len(m.group(0))
                    else:
                        raise Exception(f"Pattern parse error at {line[start:]}")


# Set randomize to False if you are going to load a file.
gol = GameOfLife(randomize=True, palette=USE_PALETTE)

# R-pentomino
# gol.board[WIDTH*30 + 128] = 0x80
# gol.board[WIDTH*30 + 129] = 0x80
# gol.board[WIDTH*31 + 127] = 0x80
# gol.board[WIDTH*31 + 128] = 0x80
# gol.board[WIDTH*32 + 128] = 0x80

# gol.load_rle("p120gun.rle")

# time.sleep(4)

gen = 0
t_frames = 0
t_total = 0
buf = memoryview(display)

# Save a couple of milliseconds since we never clear the display
display.set_blocking(False)

screen.font = rom_font.sins


while True:
    t_start = time.ticks_ms()

    gol.compute()
    t_end = time.ticks_ms()
    gol.display(buf)

    gen += 1

    if SEED_AT and gen == SEED_AT:
        gol.seed_life()
        gen = 0

    w, h = screen.measure_text(f"{SEED_AT - 1}")
    screen.pen = color.rgb(0, 0, 0, 200)
    screen.rectangle(0, 0, w + 2, h + 1)
    screen.pen = color.red
    screen.text(f"{gen}", 1, 0)
    progress = gen / (SEED_AT - 1) * (w + 2)
    screen.line(0, h, progress, h)
    display.update()
    time.sleep(0.001)

    t_end = time.ticks_ms()

    t_total += time.ticks_diff(t_end, t_start)
    t_frames += 1

    if t_frames == 100:
        per_frame_avg = t_total / t_frames
        print(f"100 frames in {t_total}ms, avg {per_frame_avg:.02f}ms per frame, {1000 / per_frame_avg:.02f} FPS")
        t_frames = 0
        t_total = 0
