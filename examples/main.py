import asyncio
import builtins
import errno
import gc
import json
import math
import os
import sys
import time

import fetch
import machine
import ntptime
import sdcard
import splash
import version
from easing import easeOutCubic
from machine import Pin
from phew import logging, redirect, render_template, serve_file, server
from scrolling import Scroll

sd_present = False
sd_logo = image.load("/assets/sd_card_icon.png")

try:
    spi = machine.SPI(0, baudrate=1_000_000, polarity=0, phase=0, sck=Pin.board.SD_SCLK, mosi=Pin.board.SD_CMD, miso=Pin.board.SD_SD0)
    sd = sdcard.SDCard(spi, Pin(Pin.board.SD_CS, Pin.OUT, value=1))
    os.mount(sd, "/sd")
    sd_present = True
except OSError:
    pass

time.sleep(1)

# Holding A and C through power-on forces provisioning.
PROVISION_BUTTONS = (Pin(BUTTON_A, Pin.IN, Pin.PULL_UP), Pin(BUTTON_C, Pin.IN, Pin.PULL_UP))
PROVISION_HOLD = 0.2


def provision_buttons_held(duration):
    held = True
    end = time.ticks_add(time.ticks_ms(), int(duration * 1000))
    while held and time.ticks_diff(end, time.ticks_ms()) > 0:
        held = not any(button.value() for button in PROVISION_BUTTONS)
        time.sleep(0.01)
    return held


force_provisioning = provision_buttons_held(PROVISION_HOLD)

# run the plash screen and store a copy of the last frame
static_splash = splash.run()

# We don't need anything related to the boot animation anymore, so we can remove it.
del splash
del sys.modules["splash"]
gc.collect()

# Override phew setting a low threshold.
gc.threshold(1024 * 1024)

# Keep the request log on the console only. Writing it takes the multicore
# lockout, so a per-request flash append stalls both cores and wears flash for
# every asset fetch.
logging.log_file = None

screen.antialias = image.X4

# pen and font read back as None until they're set, and neither setter takes
# None, so give them values before DEFAULT_STATE is captured from them.
screen.pen = color.white
screen.font = rom_font.nope

# Two drawing surfaces, so a transition gives the outgoing and incoming plugins
# one each and core1 blends them during the copy it already does. Surface 0 is
# the buffer memoryview(display) exposes, which is what `screen` wraps.
surfaces = (screen, image(screen.width, screen.height, display.surface(1)))

# An image carries its own pen, font, antialias and so on, but a surface is
# recycled once its plugin leaves the rotation. Carry the state with the plugin.
SURFACE_STATE = ("pen", "font", "antialias", "fill_rule", "alpha", "clip")

# What every plugin starts from, so what it inherits at import doesn't depend on
# which plugin happened to be imported before it.
DEFAULT_STATE = {key: getattr(screen, key) for key in SURFACE_STATE}


def capture_state(surface):
    return {key: getattr(surface, key) for key in SURFACE_STATE}


def apply_state(surface, state):
    for key, value in state.items():
        # defensive: pen and font refuse None, which is what they read back as
        # if a plugin somehow arrived without either set
        if value is not None:
            setattr(surface, key, value)

###
# Provisioning
###

loop = asyncio.get_event_loop()

needs_provisioning = force_provisioning

try:
    import config  # fails to import (missing/corrupt) go into provisioning
    if not config.provisioned:   # provisioned flag not set go into provisioning
        needs_provisioning = True
except Exception:  # noqa: BLE001
    needs_provisioning = True

if needs_provisioning:
    import provisioning    # noqa F401
    # control never returns to here, provisioning takes over completely


def migrate_config():
    defaults = {}
    try:
        with open("/defaults/config.py") as infile:
            exec(infile.read(), defaults)
    except OSError:
        print("could not read /defaults/config.py, skipping config migration")
        return

    added = [k for k in defaults if not k.startswith("_") and not hasattr(config, k)]
    for key in added:
        setattr(config, key, defaults[key])

    if added:
        print("config.py is missing", ", ".join(added), "- adding from defaults")
        with open("config.py", "a") as outfile:
            for key in added:
                outfile.write(f"\n# {key} - added in an update or restored from default\n")
                outfile.write(f"{key} = {repr(defaults[key])}\n")


# check and add new settings to the config file before reading them.
# for cases where the firmware update has added new config elements :)
migrate_config()

import wifi  # noqa E402

# How long boot waits for Wi-Fi before starting the plugins anyway
WIFI_DEADLINE = 20

# Where the connection stands. A plugin whose package.json sets
# requires_internet only runs while ONLINE.
NO_WIFI, NO_INTERNET, ONLINE = 0, 1, 2
CONNECTION_NAMES = ("no Wi-Fi", "no internet", "online")
connection = NO_WIFI

# Seconds without a link before the radio is restarted, the last repeating
RECONNECT_DELAYS = (30, 60)

# Seconds between internet checks while there's Wi-Fi but no internet. Each
# check can hold the display for a DNS lookup, so they spread out.
CHECK_DELAYS = (60, 120, 300, 900)

# The icon drawn over the plugins while not online: the splash's Wi-Fi icon on
# a half-transparent black rounded box, ICON_INSET in from the top right
ICON_INSET = 5
ICON_SCALE = 0.8
ICON_SIZE = 18
ICON_X = screen.width - ICON_INSET - ICON_SIZE
ICON_Y = ICON_INSET

# What the plugin drew under the icon, copied out before it's drawn and back
# once the frame is out. blit() blends, so it wouldn't restore exactly.
ROW_BYTES = len(memoryview(screen)) // screen.height
PATCH_BYTES = ROW_BYTES * ICON_SIZE // screen.width
PATCH_START = ICON_Y * ROW_BYTES + ROW_BYTES * ICON_X // screen.width
icon_patches = [bytearray(PATCH_BYTES * ICON_SIZE) for _ in surfaces]

# Location of plugin index
INDEX_HOST = "raw.githubusercontent.com"
INDEX_BASE = "/pimoroni/artmatrix-plugins/refs/heads/main"
INDEX_PATH = f"{INDEX_BASE}/index.json"
INDEX_CACHE = "/index-cache.json"
INDEX_INTERVAL = 15 * 60

# the server name ntptime ships with, kept before sync_time() swaps in its address
NTP_HOST = ntptime.host

# The install page's browser fetches thumbnails from the index host itself, as
# plugins that aren't installed have none on the device.
THUMBNAIL_BASE = f"https://{INDEX_HOST}{INDEX_BASE}"

FIRMWARE = version.BUILD or version.DATE

# The index is fetched in the background and served from the cache. A blocking
# requests.get() here froze the display and every other connection for the whole
# TLS round trip, on every settings page load.
index_fetch = fetch.AsyncFetch(INDEX_HOST)
INSTALL_DIR = "/.install"
install_state = None

# delay between plugins
delay = int(config.delay)

if delay >= 3600:
    delay_value, delay_unit = delay // 3600, "hours"
elif delay >= 60:
    delay_value, delay_unit = delay // 60, "minutes"
else:
    delay_value, delay_unit = delay, "seconds"
last_changed = None
current_plugin = 0
enabled_plugins = []

# Every enabled plugin as (name, module, requires_internet), and the ones in
# the rotation now as (name, module)
loaded = []
modules = []

# global brightness and setting
brightness = int(config.brightness)
display.set_brightness(config.brightness / 100)

# Per-plugin image state, and which plugin currently occupies each surface
plugin_state = {}
slot_owner = [None, None]

# Consecutive update() failures per plugin, and how many before it's skipped
plugin_failures = {}
PLUGIN_MAX_FAILURES = 5

# Plugins that draw somewhere other than the surface they were handed. They
# can't be blended, so they get a hard cut instead. Detected in run_plugin().
direct_framebuffer = set()
checked_surface_use = set()
surface_use_attempts = {}

# Plugins already reported for calling display.update(), so it's said once
warned_held = set()

# Pending config save, flushed by flush_config()
config_dirty = False
CONFIG_FLUSH_DELAY = 2

# Frames to watch a plugin for before giving up on classifying it. A frame
# limiter can skip several in a row, and a skipped frame proves nothing.
SURFACE_CHECK_FRAMES = 120

cx, cy = screen.width / 2, screen.height / 2


def import_plugin(name):
    """Import a plugin with a known starting surface state, and record what it set."""
    surface = surfaces[0]
    apply_state(surface, DEFAULT_STATE)
    builtins.screen = surface

    module = __import__(f"/lib/plugins/{name}")

    plugin_state[name] = capture_state(surface)

    # the import left arbitrary state on surface 0; force the next bind to restore
    slot_owner[0] = None

    return module


def bind(name, slot):
    """Make `name` the occupant of surface `slot` and bind it as the global screen."""
    surface = surfaces[slot]

    if slot_owner[slot] != name:
        if slot_owner[slot] is not None:
            plugin_state[slot_owner[slot]] = capture_state(surface)

        # The previous occupant's pixels are nothing to do with this plugin, and
        # not every plugin clears before it draws. Both alpha and clip bound what
        # clear() actually touches, so reset them first or a leftover value from
        # the previous occupant leaves its picture showing through.
        surface.alpha = 255
        surface.clip = rect(0, 0, surface.width, surface.height)
        surface.pen = color.black
        surface.clear()

        apply_state(surface, plugin_state.get(name, DEFAULT_STATE))

        slot_owner[slot] = name

    builtins.screen = surface


def sample(surface):
    """Cheap checksum of a surface, to tell whether a plugin drew into it."""
    buf = memoryview(surface)
    total = 0
    for offset in range(0, len(buf), 253):
        total += buf[offset]
    return total


def run_plugin(name, module, slot, verify=False):
    """Draw a plugin into surface `slot`. Returns False once it's failed too often."""
    bind(name, slot)

    # A plugin that captured a buffer at import keeps drawing into that one
    # whatever surface it is handed, and a frame that lands on surface 0 can't be
    # blended. Watch both surfaces the first time it runs on surface 1, and only
    # outside a transition, where nothing else is drawing.
    checking = verify and slot == 1 and name not in checked_surface_use
    if checking:
        before, before_other = sample(surfaces[1]), sample(surfaces[0])

    # A plugin that calls display.update() itself would commit a surface part
    # way through being drawn, and during a transition would commit the wrong
    # one. The frame belongs to draw(); hold it while the plugin renders.
    display.set_hold(True)
    try:
        module.update()
    except MemoryError:
        raise
    except Exception as e:  # noqa: BLE001
        count = plugin_failures.get(name, 0) + 1
        plugin_failures[name] = count

        # A plugin that never recovers is called every frame, so stop reporting
        # it once it has been skipped. The counter only resets on a good frame.
        if count <= PLUGIN_MAX_FAILURES:
            print(f"plugin {name} update failed ({count}/{PLUGIN_MAX_FAILURES}):")
            sys.print_exception(e)

        return count < PLUGIN_MAX_FAILURES
    finally:
        # also on the raising path, or nothing would ever flip again
        held = display.set_hold(False)
        if held and name not in warned_held:
            warned_held.add(name)
            print(f"plugin {name} calls display.update(); the frame is the host's, so it was ignored")

    plugin_failures.pop(name, None)

    if checking:
        attempts = surface_use_attempts.get(name, 0) + 1
        surface_use_attempts[name] = attempts

        if sample(surfaces[1]) != before:
            # drew where it was told
            checked_surface_use.add(name)
        elif sample(surfaces[0]) != before_other:
            print(f"plugin {name} draws outside its surface, its transitions will cut")
            direct_framebuffer.add(name)
            checked_surface_use.add(name)
        elif attempts >= SURFACE_CHECK_FRAMES:
            # drew nothing either way often enough that there's nothing to learn
            checked_surface_use.add(name)

    return True


def write_config():
    """Mark the config for saving. A flash write takes the multicore lockout, so
    coalesce the run of them a handful of settings clicks produces."""
    global config_dirty
    config_dirty = True


async def flush_config():
    global config_dirty

    while True:
        await asyncio.sleep(CONFIG_FLUSH_DELAY)
        if config_dirty:
            config_dirty = False
            save_config()


def save_config():
    lines = []
    with open("config.py", "r") as infile:
        lines = infile.read().split("\n")

    for i in range(len(lines)):
        line = lines[i]
        parts = line.split("=", 1)
        if len(parts) == 2:
            key = parts[0].strip()
            if hasattr(config, key):
                value = getattr(config, key)
                lines[i] = f"{key} = {repr(value)}"

    with open("config.py", "w") as outfile:
        outfile.write("\n".join(lines))


def load_plugins():
    global enabled_plugins, modules, loaded, delay

    delay = int(config.delay)

    plugins_on_disk = os.listdir("/lib/plugins")
    known = len(config.plugins) + len(config.disabled_plugins)

    # remove plugins that no longer exist on disk
    config.plugins = [p for p in config.plugins if p in plugins_on_disk]
    config.disabled_plugins = [p for p in config.disabled_plugins if p in plugins_on_disk]

    # add plugins that have been added to the disk, switched off
    to_add = [p for p in plugins_on_disk if p not in config.plugins]
    config.plugins += to_add
    config.disabled_plugins += to_add

    enabled_plugins = [p for p in config.plugins if p not in config.disabled_plugins]

    # and write them out if changes have been made to either remove or add
    if to_add or len(config.plugins) + len(config.disabled_plugins) != known:
        write_config()

    # drop plugins that are no longer running, before importing, so their RAM is
    # back in time for whatever is being enabled.
    modules = []
    loaded = []

    for key in list(sys.modules):
        if key.startswith("/lib/plugins/"):
            name = key[13:]
        elif key.startswith("plugins."):
            name = key.split(".")[1]
        else:
            continue

        if name not in enabled_plugins:
            del sys.modules[key]
            plugin_state.pop(name, None)
            plugin_failures.pop(name, None)

    gc.collect()

    # Import every enabled plugin, those that need the internet included while
    # there's none, so the rotation can change later without an import's pause
    for plugin in list(enabled_plugins):
        try:
            module = import_plugin(plugin)
        except (ImportError, SyntaxError, OSError, MemoryError, ValueError):

            # claw back some memory, especially if the error was memory related.
            gc.collect()

            # a bad or partial install, disabled so the settings page says so
            print(f"plugin {plugin} failed to import, disabling it:")
            enabled_plugins.remove(plugin)
            config.disabled_plugins.append(plugin)
            write_config()
            continue

        try:
            with open(f"/lib/plugins/{plugin}/package.json") as file:
                needs_internet = json.load(file).get("requires_internet", False)
        except (OSError, ValueError):
            # no manifest on the board, so it runs whatever the connection
            needs_internet = False

        loaded.append((plugin, module, needs_internet))

    # modules is empty, so this starts the rotation from the beginning
    pick_rotation()


def pick_rotation():
    """Rotate the loaded plugins that can run as things stand, without importing
    anything. The plugin showing stays on if it can still run."""
    global modules, current_plugin, fading

    showing = modules[current_plugin][0] if modules else None
    modules = [(name, module) for name, module, needs_internet in loaded
               if connection == ONLINE or not needs_internet]
    names = [entry[0] for entry in modules]

    if showing in names:
        # as in resync_modules(), a fade's indices are stale but the surfaces stand
        fading = False
        current_plugin = names.index(showing)
    else:
        current_plugin = 0

        # This runs from web handlers, so a transition may be part way through
        # and holding indices into the list just rebuilt.
        reset_transition()


def resync_modules():
    """Match the module order to enabled_plugins without re-importing anything."""
    global current_plugin, fading

    # plugins held out of the rotation keep their place for when they rejoin
    loaded.sort(key=lambda entry: enabled_plugins.index(entry[0]))

    if not modules:
        return

    showing = modules[current_plugin][0]
    modules.sort(key=lambda entry: enabled_plugins.index(entry[0]))

    # prev_plugin indexes the list as it was, so drop any transition in flight.
    # Surface ownership stands: it's the same set of plugins on the same surfaces.
    fading = False
    current_plugin = [entry[0] for entry in modules].index(showing)


def rmtree(d):
    if not d:
        raise ValueError

    for name, type, *_ in os.ilistdir(d):
        path = d + "/" + name
        if type & 0x4000:  # dir
            rmtree(path)
        else:  # file
            os.unlink(path)
    os.rmdir(d)


@server.catchall()
def catchall(request):

    # the path is pasted straight onto html/, so keep it inside that directory
    if ".." in request.path:
        return "404 Not Found Buddy!", 404

    # check if requested file exists. server.file_exists excludes directories,
    # unlike the builtin: opening one mid-response raises and drops the socket.
    file = f"html{request.path}"
    if server.file_exists(file):
        return serve_file(file)

    # requested domain was wrong
    if request.headers.get("host") != DOMAIN:
        return redirect(f"http://{DOMAIN}/wrong-host-redirect", status=302)

    return "404 Not Found Buddy!", 404


# TODO This did not seem to work for me...
@server.route("/wrong-host-redirect", methods=["GET"])
def wrong_host_redirect(_request):
    # if the client requested a resource at the wrong host then present
    # a meta redirect so that the captive portal browser can be sent to the correct location
    return f"<!DOCTYPE html><head><meta http-equiv=\"refresh\" content=\"0;URL='http://{DOMAIN}/'\" /></head>"


@server.route("/", methods=["GET"])
def device_settings(_request):
    return render_template("html/settings.html", version=FIRMWARE,
                           delay_value=str(delay_value), delay_unit=delay_unit)


@server.route("/install", methods=["GET", "POST"])
def install_page(request):
    global install_state

    if request.method == "POST":
        plugin = request.form["selected-plugin"]

        # not busy, able to install.
        if install_state is None or install_state["finished"] is True:
            # check the plugin is not already install
            if plugin not in config.plugins:
                install_state = {"plugin": plugin,
                                 "done": 0,
                                 "total": 0,
                                 "finished": False,
                                 "error": None}

                loop.create_task(install_plugin(plugin))

        return redirect("/install", status=303)

    return render_template("html/install.html", thumbnail_base=THUMBNAIL_BASE)


@server.route("/reboot", methods=["POST"])
def device_reboot(_request):

    if install_state is not None and not install_state["finished"]:
        return "A plugin is still installing. Go back and reboot once it has finished.", 409, "text/plain"

    # reset after the response has gone out, or the browser waits on a dead connection
    machine.Timer(mode=machine.Timer.ONE_SHOT, period=1000, callback=lambda _t: machine.reset())

    return render_template("html/rebooting.html")


@server.route("/synctime", methods=["POST"])
def device_sync_time(_request):

    try:
        ntptime.settime()
    except (OSError, OverflowError, ValueError):
        return "Time sync failed", 502, "text/plain"

    year, month, day, hour, minute, second = time.localtime()[:6]
    return f"{year:04d}-{month:02d}-{day:02d} {hour:02d}:{minute:02d}:{second:02d} UTC", 200, "text/plain"


@server.route("/network_status", methods=["POST"])
def device_network_status(request):

    config.network_status = request.form.get("value") == "true"
    write_config()

    return "OK", 200


@server.route("/brightness", methods=["POST"])
def device_brightness(request):

    # clamp it
    config.brightness = (max(0, min(int(request.query.get("value", 100)), 100)))

    # write it
    write_config()

    # bop it, wait no, set it!
    display.set_brightness(config.brightness / 100)

    return render_template("html/settings.html", version=FIRMWARE,
                           delay_value=str(delay_value), delay_unit=delay_unit)


DELAY_UNITS = {"seconds": 1, "minutes": 60, "hours": 3600}


@server.route("/reorder", methods=["POST"])
def reorder_plugins(request):
    global delay, delay_value, delay_unit

    if request.method == "POST":
        delay_value = int(request.form["delay_value"])
        delay_unit = request.form["delay_unit"]

        delay = delay_value * DELAY_UNITS[delay_unit]
        config.delay = delay

        write_config()

    # the settings page saves this in the background, it has nothing to show
    return "", 204


@server.route("/position", methods=["POST"])
def position(request):
    global enabled_plugins

    plugin_name = request.query.get("plugin")
    move_action = request.query.get("action")

    current_pos = config.plugins.index(plugin_name)

    if "pos" in request.query:
        new_pos = int(request.query["pos"])
    elif move_action == "up":
        new_pos = current_pos - 1
    else:
        new_pos = current_pos + 1

    if new_pos < 0:
        new_pos = 0
    elif new_pos >= len(config.plugins):
        new_pos = len(config.plugins) - 1

    config.plugins.remove(plugin_name)
    config.plugins.insert(new_pos, plugin_name)

    enabled_plugins = [p for p in config.plugins if p not in config.disabled_plugins]
    write_config()

    # the set is unchanged, so permute what's already imported and stay on the
    # plugin that's showing rather than restarting the rotation
    resync_modules()

    return "OK", 200


@server.route("/active", methods=["POST"])
def set_active_state(request):

    plugin_name = request.query.get("plugin")
    plugin_state = request.query.get("state")

    if plugin_state == "false":
        if plugin_name not in config.disabled_plugins:
            config.disabled_plugins.append(plugin_name)
    elif plugin_name in config.disabled_plugins:
        config.disabled_plugins.remove(plugin_name)

    load_plugins()
    write_config()

    # a failed import is disabled again inside load_plugins()
    if plugin_state != "false" and plugin_name in config.disabled_plugins:
        return "This plugin failed to start", 500

    return "OK", 200


@server.route("/remove", methods=["POST"])
def remove_plugin(request):

    # parse out the plugin name and create the path to remove
    plugin_name = request.query["plugin"]

    # check if the plugin is in one of the lists
    # prevents a manually written POST req removing something it shouldn't
    if plugin_name in config.plugins:
        directory = f"lib/plugins/{plugin_name}/"

        # remove the plugin directory
        rmtree(directory)

        # remove the entries from the plugin and disabled lists
        config.plugins.remove(plugin_name)
        if plugin_name in config.disabled_plugins:
            config.disabled_plugins.remove(plugin_name)

        # write this config
        write_config()
        load_plugins()

        # redirect back to the config page
        return "OK", 200
    return "404 Not Found Buddy!", 404


@server.route("/config", methods=["GET", "POST"])
def configure_plugin(request):

    plugin = request.query["plugin"]

    if plugin in config.plugins:
        p = __import__(f"/lib/plugins/{plugin}")

        # Plugins without a settings page fall back to a default
        config_page = f"lib/plugins/{plugin}/config.html"
        try:
            os.stat(config_page)
            configurable = True
        except OSError:
            config_page = "html/no-config.html"
            configurable = False

        if request.method == "POST":
            if "reset" in request.form:
                try:
                    os.remove(f"lib/plugins/{plugin}/config.json")
                except OSError:
                    print(f"{plugin} has no saved config to reset")
            else:
                # Check if the plugin has it own processor
                process = getattr(p, "process_config", None)
                if process:
                    process(request.form)
                elif configurable:
                    with open(f"lib/plugins/{plugin}/config.json", "w") as file:
                        json.dump(request.form, file)
                else:
                    # nothing to configure
                    return redirect(f"/config?plugin={plugin}", status=303)

            del sys.modules[f"/lib/plugins/{plugin}"]
            gc.collect()

            # only enabled plugins are loaded, a disabled one reloads when enabled
            for index, (name, _module, needs_internet) in enumerate(loaded):
                if name == plugin:
                    loaded[index] = (plugin, import_plugin(plugin), needs_internet)
                    pick_rotation()
                    reset_transition()
                    break
            return redirect(f"/config?plugin={plugin}", status=303)

        show_reset = configurable and not hasattr(p, "process_config")

        try:
            with open(f"/lib/plugins/{plugin}/package.json") as file:
                name = json.load(file).get("name", plugin)
        except (OSError, ValueError):
            name = plugin

        return render_template("html/config.html", plugin=plugin, name=name, placeholder=p, config_page=config_page, show_reset=show_reset)

    return "404 Not Found Buddy!", 404


@server.route("/usage.json")
def usage(_request):
    # statvfs reports fragment size, total fragments and free fragments
    frsize, blocks, bfree = os.statvfs("/")[1:4]

    # collect first or the free figure counts garbage that isn't really in use
    gc.collect()
    j = {"flash_total": frsize * blocks, "flash_free": frsize * bfree,
         "mem_total": gc.mem_alloc() + gc.mem_free(), "mem_free": gc.mem_free()}

    if sd_present:
        try:
            frsize, blocks, bfree = os.statvfs("/sd")[1:4]
            j["sd_total"] = frsize * blocks
            j["sd_free"] = frsize * bfree
        except OSError:
            pass

    return json.dumps(j), 200, "application/json"


@server.route("/plugins.json")
def plugins(_request):
    j = {"plugins": config.plugins, "disabled": config.disabled_plugins}
    return json.dumps(j), 200, "application/json"


@server.route("/index.json")
def serve_index(_request):
    # serve the cached index; watch_connection() keeps it current in the background
    try:
        with open(INDEX_CACHE, "r") as file:
            index = json.load(file)

    except (ValueError, OSError):
        # nothing cached yet, tell the page to come back
        index = {"plugins": {}, "pending": True}

    # so the pages stop waiting for the catalogue while offline
    index["offline"] = connection != ONLINE
    return json.dumps(index), 200, "application/json"


@server.route("/install.json")
def serve_install_status(_request):
    global install_state

    if install_state is None:
        return "{}", 200, "application/json"

    state = install_state
    if state["finished"]:
        install_state = None
    return json.dumps(state), 200, "application/json"


def sync_time():
    """Set the clock from NTP, which doubles as the check for the internet.
    Returns None once synced, or the error."""
    try:
        # an address, so ntptime doesn't look the name up on every check
        ntptime.host = fetch.resolve(NTP_HOST, 123)[-1][0]
        ntptime.settime()
        error = None
    except (OSError, OverflowError, ValueError) as e:
        error = e

    return error


def set_connection(state):
    global connection

    was_online = connection == ONLINE
    connection = state
    print("connection:", CONNECTION_NAMES[state])

    # plugins that need the internet join or leave the rotation
    if was_online != (state == ONLINE):
        pick_rotation()


async def watch_connection():
    lost_at = time.ticks_ms()
    reconnects = 0

    # boot has just tried NTP, so the first check waits
    check_at = time.ticks_add(time.ticks_ms(), CHECK_DELAYS[0] * 1000)
    checks = 1

    index_fetch.fetch(INDEX_PATH, file=INDEX_CACHE, interval=INDEX_INTERVAL)

    while True:
        now = time.ticks_ms()

        if not wifi.is_connected():
            if connection != NO_WIFI:
                lost_at = now
                reconnects = 0
                index_fetch.reset()
                set_connection(NO_WIFI)

            # cyw43 rejoins a short dropout by itself; a long one needs the radio restarted
            elif time.ticks_diff(now, lost_at) > RECONNECT_DELAYS[min(reconnects, len(RECONNECT_DELAYS) - 1)] * 1000:
                wifi.disconnect()
                wifi.connect()
                lost_at = time.ticks_ms()
                reconnects += 1

            await asyncio.sleep(1)
            continue

        if connection == NO_WIFI:
            check_at = now
            checks = 0
            set_connection(NO_INTERNET)

        if connection == NO_INTERNET:
            if time.ticks_diff(now, check_at) >= 0:
                error = sync_time()
                online = error is None

                # A network that blocks NTP can still reach GitHub. A failed
                # lookup, a negative lwIP code, means DNS is down, and the index
                # lookup would block for as long again.
                dns_down = isinstance(error, OSError) and isinstance(error.errno, int) and error.errno < 0
                if not online and not dns_down:
                    probe = fetch.AsyncFetch(INDEX_HOST)
                    probe.fetch(INDEX_PATH, file=INDEX_CACHE)
                    try:
                        while probe.update() == fetch.AsyncFetch.FETCHING:
                            await asyncio.sleep_ms(100)
                        online = True
                    except (OSError, ValueError, fetch.HTTPException) as e:
                        print("index check failed:", e)
                    probe.reset()

                if online:
                    set_connection(ONLINE)
                else:
                    check_at = time.ticks_add(time.ticks_ms(), CHECK_DELAYS[min(checks, len(CHECK_DELAYS) - 1)] * 1000)
                    checks += 1

            await asyncio.sleep(1)
            continue

        # Online, so keep the index cache fresh. A failed refresh leaves the
        # previous cache in place.
        try:
            index_fetch.update()
        except (OSError, ValueError, fetch.HTTPException) as e:
            print("index refresh failed:", e)

            # one unreachable host could be just that host, so check NTP as well
            if sync_time() is not None:
                check_at = time.ticks_add(time.ticks_ms(), CHECK_DELAYS[0] * 1000)
                checks = 1
                set_connection(NO_INTERNET)

        await asyncio.sleep_ms(100)


async def install_plugin(plugin):

    install_fetch = fetch.AsyncFetch(INDEX_HOST)

    plugin_base = f"{INDEX_BASE}/{plugin}/"
    temp_location = f"{INSTALL_DIR}/{plugin}"
    package_file = "package.json"

    try:
        # get the manifest for the plugin
        install_fetch.fetch(f"{plugin_base}{package_file}")

        # we can't do anything else until that file is with us.
        while install_fetch.update() == fetch.AsyncFetch.FETCHING:
            await asyncio.sleep_ms(5)

        # total number of plugin files
        j = install_fetch.to_json()
        install_state["total"] = len(j["urls"])

        try:
            os.mkdir(INSTALL_DIR)
        except OSError as e:
            if e.errno != errno.EEXIST:
                raise

        try:
            os.mkdir(temp_location)
        except OSError as e:
            if e.errno != errno.EEXIST:
                raise

        # kept with the plugin, load_plugins() reads requires_internet from it
        with open(f"{temp_location}/package.json", "w") as file:
            json.dump(j, file)

        expected_prefix = f"plugins/{plugin}/"

        # download each file listed for the requested plugin
        for dest, src in j["urls"]:
            package_file = src
            if ".." in dest or not dest.startswith(expected_prefix):
                raise ValueError
            file_path = dest[len(expected_prefix):]
            if "/" in file_path:

                # split the folders out so we can create them
                folders = file_path.split("/")

                # the last element is the file name, we don't need that.
                folders.pop()

                # go through and create the sub folders.
                current = temp_location
                for folder in folders:
                    current += f"/{folder}"
                    try:
                        os.mkdir(current)
                    except OSError as e:
                        if e.errno != errno.EEXIST:
                            raise

            # grab the next file
            install_fetch.fetch(f"{plugin_base}{src}", file=f"{temp_location}/{file_path}")
            while install_fetch.update() == fetch.AsyncFetch.FETCHING:
                await asyncio.sleep_ms(5)

            # bump the counter by one
            install_state["done"] += 1

        # now it's done, we can move the downloaded plugin to the right folder.
        os.rename(temp_location, f"/lib/plugins/{plugin}")

        config.plugins.append(plugin)
        load_plugins()

        if plugin in config.disabled_plugins:
            install_state["error"] = "installed but failed to start"
        else:
            write_config()

    except (OSError, ValueError, KeyError, fetch.HTTPException):
        try:
            # the download failed. we want to remove the folder.
            rmtree(temp_location)
        except OSError:
            pass

        install_state["error"] = f"unable to download {package_file}"

    # complete or not, we're done now. Close the connection, a later install makes its own.
    install_fetch.reset()
    install_state["finished"] = True


def draw_wifi_icon(surface, x1, y1, connected, label=None, scale=1):
    """The Wi-Fi arcs with their centre at x1, y1, drawn at scale. Unless
    connected they're crossed out in red, with label centred below."""

    radius = 10
    arc = 50
    line_angle = math.radians(arc)
    dx = ((radius * 2) / 2) * math.cos(line_angle) * scale
    dy = ((radius * 2) / 2) * math.sin(line_angle) * scale
    oy = -7 * scale
    ox = -1 * scale

    surface.pen = color.white
    for r in range(0, radius + 1, 5):
        surface.shape(shape.arc(x1, y1, r * scale, (r + 3) * scale, -arc, arc))
    surface.shape(shape.circle(x1, y1 - scale, 2 * scale))

    if not connected:
        surface.pen = color.red
        surface.shape(shape.line(x1 + ox - dx, y1 + oy - dy, x1 + ox + dx, y1 + oy + dy, 2 * scale))

        if label:
            surface.font = font.smart
            w, h = surface.measure_text(label)
            x = (surface.width / 2) - (w / 2)
            surface.pen = color.white
            surface.text(label, vec2(x, 100))


address_scrolls = {}


def show_no_plugins(title="No Plugins :("):

    screen.pen = color.rgb(0, 0, 0)
    screen.clear()

    y = 5
    screen.pen = color.white
    screen.font = rom_font.nope
    screen.text(title, 5, y)
    y += 45
    screen.text("Go to", 5, y)
    y += 15
    screen.pen = color.blue
    address = f"http://{config.wifi_hostname}.local"
    # An address too wide for the screen scrolls
    if screen.measure_text(address)[0] > screen.width - 5:
        if address not in address_scrolls:
            address_scrolls[address] = Scroll(address, rom_font.nope, align=y, gap=30)
        address_scrolls[address]()
    else:
        screen.text(address, 5, y)
    y += 15
    screen.pen = color.white
    screen.text("Or", 5, y)
    y += 15
    screen.pen = color.blue
    address = f"http://{wifi.ipv4()}"
    if screen.measure_text(address)[0] > screen.width - 5:
        if address not in address_scrolls:
            address_scrolls[address] = Scroll(address, rom_font.nope, align=y, gap=30)
        address_scrolls[address]()
    else:
        screen.text(address, 5, y)
    y += 15
    screen.pen = color.white
    screen.text("in your browser.", 5, y)


def show_wifi_failed():

    screen.pen = color.rgb(0, 0, 0)
    screen.clear()

    y = 5
    screen.pen = color.white
    screen.font = rom_font.nope
    screen.text("No Wi-Fi :(", 5, y)
    y += 25
    screen.text("Can't reach", 5, y)
    y += 15

    # A long SSID is trimmed
    name = f"{config.wifi_ssid}"
    if screen.measure_text(name)[0] > screen.width - 5:
        while name and screen.measure_text(name + "…")[0] > screen.width - 5:
            name = name[:-1]
        name += "…"
    screen.pen = color.blue
    screen.text(name, 5, y)
    y += 23
    screen.pen = color.white
    screen.text("Still trying.", 5, y)
    y += 15
    screen.text("Hold A+C and", 5, y)
    y += 15
    screen.text("RESET to change", 5, y)
    y += 15
    screen.text("Wi-Fi details.", 5, y)


def wait_for_wifi(deadline):
    while not wifi.is_connected() and time.ticks_diff(deadline, time.ticks_ms()) > 0:
        wifi.tick()
        time.sleep_ms(50)
    return wifi.is_connected()


FADE_DURATION = 500
fade_start = 0
fading = False
current_slot = 0
prev_plugin = 0
prev_slot = 1

# A fade renders both plugins per frame. Where that pair is too expensive the
# outgoing one holds its last frame instead: it is the one being left, and a
# steady frame rate reads better than its final half second of animation.
FRAME_BUDGET_MS = 25
frame_ms = 0
fade_animate_prev = True


def reset_transition():
    global fading, current_slot, prev_slot, last_changed
    fading = False
    current_slot = 0
    prev_slot = 1
    slot_owner[0] = slot_owner[1] = None
    last_changed = time.time()


async def flip(blend, slots=()):
    """Commit a frame, then wait for core1 before anything draws again. While
    not online the icon goes over the surfaces in slots, for this frame only."""
    covered = slots if connection != ONLINE and config.network_status else ()

    # At full size the arcs reach 10 px either side of x1, the cross 16 px above
    # y1 and the dot 1 px below, so this centres the icon in its box, a pixel
    # high by eye
    x1 = ICON_X + ICON_SIZE // 2
    y1 = ICON_Y + round(17 * ICON_SCALE)
    s = ICON_SCALE

    for slot in covered:
        surface = surfaces[slot]
        buffer = memoryview(surface)
        patch = icon_patches[slot]
        for row in range(ICON_SIZE):
            start = PATCH_START + row * ROW_BYTES
            patch[row * PATCH_BYTES:(row + 1) * PATCH_BYTES] = buffer[start:start + PATCH_BYTES]

        # the surface's pen, clip and so on are the plugin's
        state = capture_state(surface)
        surface.alpha = 255
        surface.antialias = image.X4
        surface.clip = rect(ICON_X, ICON_Y, ICON_SIZE, ICON_SIZE)
        surface.pen = color.rgb(0, 0, 0, 128)
        surface.shape(shape.rounded_rectangle(ICON_X, ICON_Y, ICON_SIZE, ICON_SIZE, 3, 3, 3, 3))

        draw_wifi_icon(surface, x1, y1, connection != NO_WIFI, scale=s)

        if connection == NO_INTERNET:
            # over the right of the arcs, with a black edge to keep it apart from them
            surface.pen = color.black
            surface.shape(shape.rectangle(x1 + 5 * s, y1 - 14 * s, 4 * s, 16 * s))
            surface.pen = color.rgb(255, 160, 0)
            surface.shape(shape.rectangle(x1 + 6 * s, y1 - 13 * s, 2 * s, 9 * s))
            surface.shape(shape.rectangle(x1 + 6 * s, y1 - 2 * s, 2 * s, 2 * s))

        apply_state(surface, state)

    display.update(blend)

    # Waiting here rather than after the next draw is what keeps the clear from
    # chasing the flip, and the awaits let the web server run meanwhile.
    while display.is_busy():
        await asyncio.sleep_ms(0)

    # the plugin's next frame starts from the pixels it drew
    for slot in covered:
        buffer = memoryview(surfaces[slot])
        patch = icon_patches[slot]
        for row in range(ICON_SIZE):
            start = PATCH_START + row * ROW_BYTES
            buffer[start:start + PATCH_BYTES] = patch[row * PATCH_BYTES:(row + 1) * PATCH_BYTES]


def start_transition():
    global current_plugin, current_slot, prev_plugin, prev_slot, fade_start, fading, last_changed
    global fade_animate_prev

    prev_plugin = current_plugin
    prev_slot = current_slot

    current_plugin = (current_plugin + 1) % len(modules)

    outgoing_direct = modules[prev_plugin][0] in direct_framebuffer

    if modules[current_plugin][0] in direct_framebuffer:
        # it writes surface 0 whatever surface it is handed, so that is its slot
        current_slot = 0
    else:
        current_slot = 1 - prev_slot

    # Two plugins can't share surface 0, and a direct one can't be moved off it,
    # so those transitions cut instead of blending.
    fading = current_slot != prev_slot and not (outgoing_direct and prev_slot != 0)

    # Decided once, from the frame rate going in, so it can't oscillate between
    # animating and holding across the fade.
    fade_animate_prev = frame_ms <= FRAME_BUDGET_MS

    if fading:
        # Draw the incoming plugin before any of it is shown. bind() clears its
        # surface, and a frame limiter can skip the first update, so otherwise
        # the fade opens by blending toward black.
        run_plugin(modules[current_plugin][0], modules[current_plugin][1], current_slot)

    fade_start = time.ticks_ms()
    last_changed = time.time()

    gc.collect()


async def draw():
    global fading, current_slot, frame_ms

    reset_transition()

    while True:
        frame_start = time.ticks_ms()

        if modules:
            name, module = modules[current_plugin]

            if fading:
                progress = min(1.0, time.ticks_diff(time.ticks_ms(), fade_start) / FADE_DURATION)
                mix = int(255 * easeOutCubic(progress))

                # Both plugins animate through the fade; core1 composites. The
                # outgoing one holds its last frame if the pair costs too much.
                if fade_animate_prev:
                    prev_name, prev_module = modules[prev_plugin]
                    run_plugin(prev_name, prev_module, prev_slot)
                run_plugin(name, module, current_slot)

                if progress >= 1.0:
                    fading = False

                await flip(mix if current_slot else 255 - mix, (0, 1))
            else:
                healthy = run_plugin(name, module, current_slot, verify=True)

                # just found to write surface 0, so show that rather than the
                # surface it was handed and never touched
                if current_slot and name in direct_framebuffer:
                    current_slot = 0

                await flip(255 if current_slot else 0, (current_slot,))

                due = (time.time() - last_changed) > delay
                if (due or not healthy) and len(modules) > 1:
                    start_transition()
        else:
            builtins.screen = surfaces[0]
            # the surface belongs to no plugin now, so the next bind restores fully
            slot_owner[0] = None
            if connection == NO_WIFI:
                show_wifi_failed()
            elif connection == NO_INTERNET and enabled_plugins:
                # everything switched on needs the internet
                show_no_plugins("No Internet :(")
            else:
                show_no_plugins()
            await flip(0)

        frame_ms = time.ticks_diff(time.ticks_ms(), frame_start)

        await asyncio.sleep_ms(10)

leds(color.rgb(255, 120, 0))

screen.blit(static_splash, vec2(0, 0))
draw_wifi_icon(screen, screen.width / 2, 23, False, "Connecting")
display.update()

# One attempt, then the plugins start whatever happened. watch_connection()
# carries on from there.
wifi.connect()
if wait_for_wifi(time.ticks_add(time.ticks_ms(), WIFI_DEADLINE * 1000)):
    connection = ONLINE if sync_time() is None else NO_INTERNET

print("connection:", CONNECTION_NAMES[connection])

leds(color.rgb(0, 255, 0) if wifi.is_connected() else color.rgb(255, 0, 0))


try:
    os.mkdir("/lib")
    os.mkdir("/lib/plugins")
except OSError:
    pass

try:
    rmtree(INSTALL_DIR)
except OSError:
    pass

load_plugins()

# blit the last frame of the splash as a "clear"
screen.blit(static_splash, vec2(0, 0))

# draw the connected icon and update the display
draw_wifi_icon(screen, screen.width / 2, 23, wifi.is_connected(), "Connecting")
if sd_present:
    screen.blit(sd_logo, 5, 5)
display.update()

# from here the corner icon shows the connection
leds.clear()

del sd_logo

# we're done with the splash frame so we can remove it now.
del static_splash

print(config.wifi_hostname)
print(wifi.ipv4())
DOMAIN = f"{config.wifi_hostname}.local"

# Core1 owns the surfaces between update() and the flip completing. Up to here
# the blocking wait is what keeps the next clear off a frame core1 is still
# reading; from here draw() waits cooperatively so the web server can run.
display.set_blocking(False)

loop.create_task(draw())
loop.create_task(watch_connection())
loop.create_task(flush_config())
server.run(host="0.0.0.0", port=80)
