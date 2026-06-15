from phew import server, redirect, serve_file, render_template
import time
import asyncio
import mip
import requests
import json
import os
import sys
import gc

###
# Provisioning
###

loop = asyncio.get_event_loop()

needs_provisioning = False

try:
    import config   # fails to import (missing/corrupt) go into provisioning
    if not config.provisioned:   # provisioned flag not set go into provisioning
        needs_provisioning = True
except Exception:
    needs_provisioning = True

if needs_provisioning:
    import provisioning    # noqa F401
    # control never returns to here, provisioning takes over completely

import wifi     # noqa E402

# Location of plugin index
INDEX_URL = "https://raw.githubusercontent.com/pimoroni/monkey-banana/refs/heads/main/index.json"

# delay between plugins
delay = int(config.delay)
last_changed = time.time()
current_plugin = 0
enabled_plugins = []
NUM_PLUGINS = 0
modules = []

cx, cy = screen.width / 2, screen.height / 2


def load_plugins():
    global enabled_plugins, NUM_PLUGINS, modules, current_plugin, delay

    enabled_plugins = config.enabled_plugins
    delay = int(config.delay)

    NUM_PLUGINS = len(enabled_plugins)
    modules = []
    for plugin in enabled_plugins:
        modules.append((f"{plugin}", __import__(f"lib/plugins/{plugin}")))

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

        config.enabled_plugins = [k.lstrip("plugin_") for k in request.form["enabled_ordered"].split(",") if k]
        config.disabled_plugins = [k.lstrip("plugin_") for k in request.form["disabled_ordered"].split(",") if k]
        config.delay = request.form["delay_time"]

        write_config()
        load_plugins()

    return redirect(f"http://{DOMAIN}/")


@server.route("/remove")
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
    return redirect(f"http://{DOMAIN}/")


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


async def draw():
    global last_changed, current_plugin

    while True:

        screen.pen = color.rgb(0, 0, 0)
        screen.clear()

        if modules:

            modules[current_plugin][1].update()

            if (time.time() - last_changed) > delay:
                current_plugin += 1
                current_plugin %= NUM_PLUGINS
                last_changed = time.time()
        else:
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

        display.update()

        await asyncio.sleep_ms(10)

screen.pen = color.black
screen.clear()
draw_wifi_icon(color.red)
display.update()

wifi.connect()
while not wifi.is_connected():
    wifi.tick()

load_plugins()

screen.pen = color.black
screen.clear()
draw_wifi_icon(color.green)
display.update()

DOMAIN = f"{config.wifi_hostname}.local"

loop.create_task(draw())
server.run(host="0.0.0.0", port=80)
