import asyncio
import gc
import json
import os
import sys
import time

import fetch
import mip
import ntptime
from easing import easeOutCubic
from phew import logging, redirect, render_template, serve_file, server

# Keep the request log on the console only. Writing it takes the multicore
# lockout, so a per-request flash append stalls both cores and wears flash for
# every asset fetch.
logging.log_file = None

screen.antialias = image.X4

###
# Provisioning
###

loop = asyncio.get_event_loop()

needs_provisioning = False

try:
    import config  # fails to import (missing/corrupt) go into provisioning
    if not config.provisioned:   # provisioned flag not set go into provisioning
        needs_provisioning = True
except Exception:  # noqa: BLE001
    needs_provisioning = True

if needs_provisioning:
    import provisioning    # noqa F401
    # control never returns to here, provisioning takes over completely

import wifi  # noqa E402

# Location of plugin index
INDEX_HOST = "raw.githubusercontent.com"
INDEX_PATH = "/pimoroni/monkey-banana/refs/heads/main/index.json"
INDEX_CACHE = "/index-cache.json"
INDEX_INTERVAL = 15 * 60

# The index is fetched in the background and served from the cache. A blocking
# requests.get() here froze the display and every other connection for the whole
# TLS round trip, on every settings page load.
index_fetch = fetch.AsyncFetch(INDEX_HOST)

# delay between plugins
delay = int(config.delay)
last_changed = None
current_plugin = 0
enabled_plugins = []
disabled_plugins = []
NUM_PLUGINS = 0
modules = []

# Pending config save, flushed by flush_config()
config_dirty = False
CONFIG_FLUSH_DELAY = 2

cx, cy = screen.width / 2, screen.height / 2


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
    global enabled_plugins, disabled_plugins, NUM_PLUGINS, modules, current_plugin, delay

    enabled_plugins = config.enabled_plugins
    disabled_plugins = config.disabled_plugins
    delay = int(config.delay)

    plugins_on_disk = os.listdir("/lib/plugins")
    combined = enabled_plugins + disabled_plugins

    # remove plugins that no longer exist on disk
    enabled_plugins = [p for p in enabled_plugins if p in plugins_on_disk]
    disabled_plugins = [p for p in disabled_plugins if p in plugins_on_disk]

    # add plugins that have been added to the disk
    to_add = list(set(plugins_on_disk) - set(combined))
    disabled_plugins += to_add

    # save back to config
    config.enabled_plugins = enabled_plugins
    config.disabled_plugins = sorted(disabled_plugins)

    # and write them out if changes have been made to either remove or add
    if to_add or len(enabled_plugins) + len(disabled_plugins) != len(combined):
        write_config()

    # import enabled plugins
    NUM_PLUGINS = len(enabled_plugins)
    modules = [(plugin, __import__(f"/lib/plugins/{plugin}")) for plugin in enabled_plugins]

    # reset the current to zero, start from the beginning.
    current_plugin = 0


def resync_modules():
    """Match the module order to enabled_plugins without re-importing anything."""
    global current_plugin, fading

    if not modules:
        return

    showing = modules[current_plugin][0]
    modules.sort(key=lambda entry: enabled_plugins.index(entry[0]))

    # prev_plugin indexes the list as it was, so drop any transition in flight
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


@server.route("/", methods=["GET", "POST"])
def device_settings(request):
    if request.method == "POST":
        plugin = request.form["selected-plugin"]
        install_url = f"github:pimoroni/monkey-banana/{plugin}"
        mip.install(install_url)

        # add the newly installed plugin to the enabled list and save it.
        if plugin not in config.enabled_plugins:
            config.enabled_plugins.append(plugin)
            write_config()
            load_plugins()
    return render_template("html/settings.html")


@server.route("/reorder", methods=["POST"])
def reorder_plugins(request):
    global delay

    if request.method == "POST":
        # only the dwell time, so nothing needs re-importing
        config.delay = request.form["delay_time"]
        delay = int(config.delay)

        write_config()

    return redirect("/")


@server.route("/position", methods=["POST"])
def position(request):

    plugin_name = request.query.get("plugin")
    move_action = request.query.get("action")

    current_pos = enabled_plugins.index(plugin_name)

    if move_action == "up":
        new_pos = current_pos - 1
    else:
        new_pos = current_pos + 1

    if new_pos < 0:
        new_pos = 0
    elif new_pos >= len(enabled_plugins):
        new_pos = len(enabled_plugins) - 1

    enabled_plugins.remove(plugin_name)
    enabled_plugins.insert(new_pos, plugin_name)

    config.enabled_plugins = enabled_plugins
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
        if plugin_name in enabled_plugins:
            enabled_plugins.remove(plugin_name)
            disabled_plugins.append(plugin_name)
    else:
        if plugin_name in disabled_plugins:
            disabled_plugins.remove(plugin_name)
            enabled_plugins.append(plugin_name)

    config.enabled_plugins = enabled_plugins
    config.disabled_plugins = disabled_plugins
    write_config()
    load_plugins()

    return "OK", 200


@server.route("/remove", methods=["POST"])
def remove_plugin(request):

    # parse out the plugin name and create the path to remove
    plugin_name = request.query["plugin"]
    directory = f"lib/plugins/{plugin_name}/"

    # remove the plugin directory
    rmtree(directory)

    # remove the entries from the enabled/disaabled lists
    if plugin_name in config.enabled_plugins:
        config.enabled_plugins.remove(plugin_name)
    if plugin_name in config.disabled_plugins:
        config.disabled_plugins.remove(plugin_name)

    # write this config
    write_config()
    load_plugins()

    # redirect back to the config page
    return "OK", 200


@server.route("/config", methods=["GET", "POST"])
def configure_plugin(request):

    plugin = request.query["plugin"]
    p = __import__(f"/lib/plugins/{plugin}")

    if request.method == "POST":
        # Check if the plugin has it own processor
        process = getattr(p, "process_config", None)
        if process:
            process(request.form)
        else:
            with open(f"lib/plugins/{plugin}/config.json", "w") as file:
                json.dump(request.form, file)

        del sys.modules[f"/lib/plugins/{plugin}"]
        gc.collect()
        index = [modules.index(m) for m in modules if m[0] == plugin][0]
        modules[index] = (f"{plugin}", __import__(f"/lib/plugins/{plugin}"))

        return redirect(f"/config?plugin={plugin}", status=303)

    return render_template("html/config.html", plugin=plugin, placeholder=p)


@server.route("/plugins.json")
def plugins(_request):
    j = {"enabled": config.enabled_plugins, "disabled": config.disabled_plugins}
    return json.dumps(j), 200, "application/json"


@server.route("/index.json")
def networks(_request):
    # serve the cached index; refresh_index() keeps it current in the background
    try:
        with open(INDEX_CACHE, "r") as file:
            return file.read(), 200, "application/json"
    except OSError:
        # nothing cached yet, tell the page to come back
        return json.dumps({"plugins": [], "pending": True}), 200, "application/json"


async def refresh_index():
    index_fetch.fetch(INDEX_PATH, file=INDEX_CACHE, interval=INDEX_INTERVAL)

    while True:
        try:
            index_fetch.update()
            await asyncio.sleep_ms(100)
        except Exception as e:  # noqa: BLE001
            # a failed refresh leaves the previous cache in place. Back off, or a
            # persistent failure retries every tick: update() only stamps the
            # interval on a completed fetch.
            print("index refresh failed:", e)
            await asyncio.sleep(60)


def draw_wifi_icon(col):
    screen.pen = col
    wifi_shape = shape.pie(cx, cy + 30, 50, -45, 45)
    wifi_outline = shape.pie(cx, cy + 30, 50, -45, 45).stroke(4)
    screen.shape(wifi_shape)
    screen.pen = color.white
    screen.shape(wifi_outline)


def show_no_plugins():

    screen.pen = color.rgb(0, 0, 0)
    screen.clear()

    y = 5
    screen.pen = color.white
    screen.font = rom_font.nope
    screen.text("No Plugins :(", 5, y)
    y += 45
    screen.text("Go to", 5, y)
    y += 15
    screen.pen = color.blue
    screen.text(f"{config.wifi_hostname}.local", 5, y)
    y += 15
    screen.pen = color.white
    screen.text("Or", 5, y)
    y += 15
    screen.pen = color.blue
    screen.text(f"{wifi.ipv4()}", 5, y)
    y += 15
    screen.pen = color.white
    screen.text("in your browser.", 5, y)


FADE_DURATION = 500
fade_start = 0
fading = False
prev_plugin = None
next_plugin = None


async def draw():
    global last_changed, current_plugin, prev_plugin, next_plugin, fade_start, fading

    last_changed = time.time()
    screen.pen = color.rgb(0, 0, 0)
    screen.clear()

    while True:

        if modules:
            if fading:
                fade_progress = min(1.0, time.ticks_diff(time.ticks_ms(), fade_start) / FADE_DURATION)
                t = int(255 * easeOutCubic(fade_progress))

                screen.alpha = 255
                screen.blit(prev_plugin, vec2(0, 0))
                screen.alpha = t
                screen.blit(next_plugin, vec2(0, 0))

                if t >= 255:
                    fading = False
                    screen.alpha = 255
                    del prev_plugin, next_plugin
                    gc.collect()

                display.update()
            else:
                modules[current_plugin][1].update()
                display.update()

                if (time.time() - last_changed) > delay and len(modules) > 1:
                    gc.collect()

                    prev_plugin = image(screen.width, screen.height)
                    prev_plugin.blit(screen, vec2(0, 0))

                    current_plugin += 1
                    current_plugin %= NUM_PLUGINS

                    modules[current_plugin][1].update()

                    next_plugin = image(screen.width, screen.height)
                    next_plugin.blit(screen, vec2(0, 0))

                    fade_start = time.ticks_ms()
                    last_changed = time.time()
                    fading = True
        else:
            show_no_plugins()
            display.update()

        await asyncio.sleep_ms(10)

screen.pen = color.black
screen.clear()
draw_wifi_icon(color.red)
display.update()

wifi.connect()
while not wifi.is_connected():
    wifi.tick()

# ignore OSError and continue
try:
    ntptime.settime()
except OSError:
    pass


try:
    os.mkdir("/lib")
    os.mkdir("/lib/plugins")
except OSError:
    pass

load_plugins()

screen.pen = color.black
screen.clear()
draw_wifi_icon(color.green)
display.update()

print(config.wifi_hostname)
DOMAIN = f"{config.wifi_hostname}.local"

loop.create_task(draw())
loop.create_task(refresh_index())
loop.create_task(flush_config())
server.run(host="0.0.0.0", port=80)
