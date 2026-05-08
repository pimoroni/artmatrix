from lib.phew import server, redirect, serve_file, render_template
import wifi
import time
import asyncio
import mip
import requests
import json
import os

###
# Provisioning
###

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

# Location of plugin index
INDEX_URL = "https://raw.githubusercontent.com/pimoroni/monkey-banana/refs/heads/main/index.json"

# delay between plugins
DELAY = config.delay
last_changed = time.time()
current_plugin = 0

# Wifi connect
# Will probably change at some point
screen.pen = color.red
screen.clear()
display.update()
print("Connecting...")
wifi.connect()
while not wifi.is_connected():
    wifi.tick()
screen.pen = color.green
screen.clear()
display.update()

DOMAIN = wifi.ipv4()

# list of installed plugins
# and import the modules, sorted in a list
if config.enabled_plugins:
    enabled_plugins = config.enabled_plugins
else:
    try:
        # get a list of all the plugins current_pluginly installed
        enabled_plugins = [p for p in os.listdir("lib/plugins")]
    except OSError:
        pass   # We want to handle this case of no plugins being installed.

NUM_PLUGINS = len(enabled_plugins)
modules = []
for plugin in enabled_plugins:
    modules.append(__import__(f"lib/plugins/{plugin}"))


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
    return f"<!DOCTYPE html><head><meta http-equiv=\"refresh\" content=\"0;URL='http://{DOMAIN}/config'\" /></head>"


@server.route("/config", methods=["GET", "POST"])
def edit_config(request):
    if request.method == "POST":
        plugin = request.form["selected-plugin"]
        install_url = f"github:pimoroni/monkey-banana/{plugin}"
        mip.install(install_url)
    return render_template("html/edit-config.html")


@server.route("/reorder", methods=["POST"])
def reorder_plugins(request):

    print(request.form["enabled_ordered"], request.form["disabled_ordered"])

    if request.method == "POST":
        config.enabled_plugins = [kv[1] for kv in [kv.split("=") for kv in request.form["enabled_ordered"].split("&")] if kv[0] == "plugin"]
        config.disabled_plugins = [kv[1] for kv in [kv.split("=") for kv in request.form["disabled_ordered"].split("&")] if kv[0] == "plugin"]

        write_config()

        print(config.enabled_plugins, config.enabled_plugins)

    return redirect(f"http://{DOMAIN}/config")


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


# temp
# drawing a bouncy squircle to see display + web server
async def draw():
    global last_changed, current_plugin
    while True:
        if (time.time() - last_changed) > DELAY:
            current_plugin += 1
            current_plugin %= NUM_PLUGINS
            last_changed = time.time()

        modules[current_plugin].update()
        display.update()

        await asyncio.sleep(0.01)


asyncio.create_task(draw())
while True:
    server.run(host="0.0.0.0", port=80)
