import asyncio
import gc
import json
import os
import sys
import time

import mip
import ntptime
import requests
from easing import easeOutCubic
from phew import redirect, render_template, serve_file, server

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
except Exception:
    needs_provisioning = True

if needs_provisioning:
    import provisioning    # noqa F401
    # control never returns to here, provisioning takes over completely

import wifi  # noqa E402

# Location of plugin index
INDEX_URL = "https://raw.githubusercontent.com/pimoroni/monkey-banana/refs/heads/main/index.json"

# delay between plugins
delay = int(config.delay)
last_changed = None
current_plugin = 0
enabled_plugins = []
disabled_plugins = []
NUM_PLUGINS = 0
modules = []

cx, cy = screen.width / 2, screen.height / 2


def load_plugins():
    global enabled_plugins, disabled_plugins, NUM_PLUGINS, modules, current_plugin, delay

    enabled_plugins = config.enabled_plugins
    disabled_plugins = config.disabled_plugins
    delay = int(config.delay)

    NUM_PLUGINS = len(enabled_plugins)
    modules = []
    for plugin in enabled_plugins:
        modules.append((f"{plugin}", __import__(f"/lib/plugins/{plugin}")))

    # reset the current to zero, start from the beginning.
    current_plugin = 0


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


def write_config():
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


@server.catchall()
def catchall(request):
    # requested domain was wrong
    if request.headers.get("host") != DOMAIN:
        return redirect(f"http://{DOMAIN}/wrong-host-redirect")

    # check if requested file exists
    file = f"html{request.path}"
    if file_exists(file):
        return serve_file(file)

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
    if request.method == "POST":

        config.delay = request.form["delay_time"]

        write_config()
        load_plugins()

    return redirect(f"http://{DOMAIN}/")


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
    load_plugins()

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
    p = __import__(f"lib/plugins/{plugin}")

    if request.method == "POST":
        with open(f"lib/plugins/{plugin}/config.json", "w") as file:
            json.dump(request.form, file)

        del sys.modules[f"lib/plugins/{plugin}"]
        gc.collect()
        index = [modules.index(m) for m in modules if m[0] == plugin][0]
        modules[index] = (f"{plugin}", __import__(f"lib/plugins/{plugin}"))

    try:
        return p.config
    except AttributeError:
        return redirect(f"http://{DOMAIN}/")


@server.route("/plugins.json")
def plugins(request):
    j = {"enabled": config.enabled_plugins, "disabled": config.disabled_plugins}
    return json.dumps(j), 200, "application/json"


@server.route("/index.json")
def networks(request):
    # get the index of available plugins and return it
    r = requests.get(INDEX_URL)
    j = r.json()
    return json.dumps(j), 200, "application/json"


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
    y += 75
    screen.text("Go to", 5, y)
    y += 15
    screen.pen = color.blue
    screen.text(f"{config.wifi_hostname}.local", 5, y)
    y += 15
    screen.pen = color.white
    screen.text("in your browser.", 5, y)


FADE_DURATION = 200
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

load_plugins()

screen.pen = color.black
screen.clear()
draw_wifi_icon(color.green)
display.update()

print(config.wifi_hostname)
DOMAIN = f"{config.wifi_hostname}.local"

loop.create_task(draw())
server.run(host="0.0.0.0", port=80)
