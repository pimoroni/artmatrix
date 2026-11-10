import json
import time
import asyncio
import machine
from phew import logging, server, redirect, serve_file, render_template, access_point
import network

wlan = None
test_state = "IDLE"
test_ssid = ""
test_password = ""
test_hostname = ""
test_time_zone = ""
test_started = 0

# Console only. The wifi_test_done poll runs at 1Hz, and each flash append takes
# the multicore lockout.
logging.log_file = None

DOMAIN = "artmatrix.wireless"
ssid = "Art Matrix Setup"

TEST_DEADLINE_MS = 30000
RESULT_HOLD_MS = 3000
PHONE_POLL_MS = 1000

WAITING = "Waiting for user"

# the line under SETUP MODE, and whether a phone is on the setup network.
# main.py shows the setup screen while a phone is on or a test is running
status_line = WAITING
phone_joined = False

# main.py imports this with the splash on screen
background = image(screen.width, screen.height)
background.blit(screen, vec2(0, 0))


# sized like main.py's plugin name pill, flat on the left edge and round on the right
PILL_HEIGHT = 17
PILL_PAD = 4
PILL_INSET = 5


# main.py calls this with screen bound to surface 0, still in the state the last
# plugin left it
def draw():
    screen.alpha = 255
    screen.clip = rect(0, 0, screen.width, screen.height)
    screen.blit(background, vec2(0, 0))

    screen.antialias = image.X4
    screen.font = rom_font.nope
    radius = PILL_HEIGHT / 2
    for text, y in (("SETUP MODE", PILL_INSET), (status_line, screen.height - PILL_INSET - PILL_HEIGHT)):
        width = screen.measure_text(text)[0] + PILL_PAD + PILL_HEIGHT // 2
        screen.pen = color.rgb(0, 0, 0, 128)
        screen.shape(shape.rounded_rectangle(0, y, width, PILL_HEIGHT, 0, radius, radius, 0))
        screen.pen = color.rgb(255, 255, 255, 191)
        screen.text(text, PILL_PAD, y + 2)


# put board into access point mode
logging.info("> going into access point mode")
ap = access_point(ssid, password="")
logging.info("  -", ap.ifconfig()[0])

# dns server to catch all dns requests
logging.info("> starting dns server...")
from phew import dns   # noqa E402
dns.run_catchall(ap.ifconfig()[0])

logging.info("> creating web server...")


# write the current values in config to the config.py file
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


import config   # noqa E402
import timezones   # noqa E402

HOSTNAME_CHARS = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-"


# same rule as the name field's pattern, for requests that skip the form
def valid_hostname(name):
    if not 0 < len(name) <= 32 or name[0] == "-" or name[-1] == "-":
        return False
    return all(c in HOSTNAME_CHARS for c in name)


@server.route("/provision-welcome", methods=["GET", "POST"])
def provision_welcome(request):
    if request.method == "POST":
        ssid = request.form.get("wifi_ssid", "")
        password = request.form.get("wifi_password", "")
        hostname = request.form.get("wifi_hostname", "")
        if ssid and password and valid_hostname(hostname):
            config.wifi_ssid = ssid
            config.wifi_password = password
            config.wifi_hostname = hostname
            zone = request.form.get("time_zone", "")
            if zone in [row[0] for row in timezones.ZONES]:
                config.time_zone = zone
            config.provisioned = True
            write_config()
            machine.reset()
    return render_template("html/provision-wifi.html", time_zones=json.dumps(timezones.choices()))


@server.route("/test-network", methods=["POST"])
def test_network(request):
    global wlan, test_state, test_ssid, test_password, test_hostname, test_time_zone, test_started, status_line
    if test_state == "CONNECTING":
        return "OK"

    test_ssid = request.form.get("wifi_ssid", "")
    test_password = request.form.get("wifi_password", "")
    test_hostname = request.form.get("wifi_hostname", "")
    test_time_zone = request.form.get("time_zone", "")
    if not test_ssid or not valid_hostname(test_hostname):
        return "FAILED"

    status_line = "Testing WiFi"

    if wlan is None:
        wlan = network.WLAN(network.STA_IF)
    wlan.active(True)
    wlan.disconnect()
    wlan.connect(test_ssid, test_password)
    test_started = time.ticks_ms()
    test_state = "CONNECTING"
    return "OK"


@server.route("/wifi_test_done", methods=["GET"])
def provisioning_done(_request):
    return test_state


async def watch_phone():
    global phone_joined
    while True:
        phone_joined = bool(ap.status("stations"))
        await asyncio.sleep_ms(PHONE_POLL_MS)


async def watch_test():
    global test_state, status_line
    while True:
        await asyncio.sleep_ms(250)
        if test_state != "CONNECTING":
            continue

        status = wlan.status()
        if status == network.STAT_GOT_IP:
            test_state = "OK"
            config.wifi_ssid = test_ssid
            config.wifi_password = test_password
            if test_hostname:
                config.wifi_hostname = test_hostname
            if test_time_zone in [row[0] for row in timezones.ZONES]:
                config.time_zone = test_time_zone
            config.provisioned = True
            write_config()
            status_line = "Connected!"
            await asyncio.sleep_ms(RESULT_HOLD_MS)
            machine.reset()

        if status < 0 or time.ticks_diff(time.ticks_ms(), test_started) > TEST_DEADLINE_MS:
            test_state = "FAILED"
            wlan.disconnect()
            status_line = "Couldn't connect"
            await asyncio.sleep_ms(RESULT_HOLD_MS)
            if test_state == "FAILED":
                status_line = WAITING


@server.route("/networks.json")
def networks(_request):
    networks = []
    for net in ap.scan():
        net = net[0].decode("ascii").strip()
        if net != "":
            networks.append(net)
    networks = list(set(networks))   # remove duplicates
    return json.dumps(networks), 200, "application/json"


@server.catchall()
def catchall(request):
    # requested domain was wrong
    if request.headers.get("host") != DOMAIN:
        return redirect(f"http://{DOMAIN}/provision-welcome", status=302)

    # the path is pasted straight onto html/, so keep it inside that directory
    if ".." in request.path:
        return "404 Not Found Buddy!", 404

    # check if requested file exists. server.file_exists excludes directories,
    # unlike the builtin: opening one mid-response raises and drops the socket.
    file = f"html{request.path}"
    if server.file_exists(file):
        return serve_file(file)

    return "404 Not Found Buddy!", 404


# main.py runs the server, these run alongside its frame loop
server.loop.create_task(watch_phone())
server.loop.create_task(watch_test())
