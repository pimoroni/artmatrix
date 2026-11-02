import os
import json
import time
import asyncio
import machine
from phew import logging, server, redirect, serve_file, render_template, access_point
import network
from easing import easeInOutSine

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

screen.font = rom_font.nope

DOMAIN = "artmatrix.wireless"
ssid = "Art Matrix Setup"
ssid_length, _ = screen.measure_text(ssid)

# create the qr payload
qr_payload = f"WIFI:S:{ssid};T:nopass;;"

FADE_MS = 500
TEST_DEADLINE_MS = 30000
RESULT_HOLD_MS = 3000

background = image(screen.width, screen.height)
background.blit(screen, vec2(0, 0))

# qr() draws dark on light, so swap the palette to suit this splash.
qr = image.qr(qr_payload, image.QR_MEDIUM, 0)
qr.palette = [color.rgb(0, 0, 0), color.white]
qr_size = qr.width * 3
qr_x = (screen.width - qr_size) // 2


def fit(text):
    if screen.measure_text(text)[0] <= screen.width - 10:
        return text
    while text and screen.measure_text(text + "...")[0] > screen.width - 10:
        text = text[:-1]
    return text + "..."


def draw_setup():
    screen.blit(background, vec2(0, 0))
    screen.pen = color.rgb(0, 0, 0, 75)
    screen.clear()
    screen.pen = color.rgb(0, 0, 0)
    screen.rectangle(qr_x - 4, 4, qr_size + 8, qr_size + 8)
    screen.blit(qr, rect(qr_x, 8, qr_size, qr_size))
    screen.pen = color.white
    screen.text("Connect to:", 5, screen.height - 29)
    screen.text(f"{ssid}", 5, screen.height - 16)


def draw_message(title, detail):
    screen.blit(background, vec2(0, 0))
    screen.pen = color.rgb(0, 0, 0, 180)
    screen.clear()
    screen.pen = color.white
    screen.text(title, 5, screen.height - 29)
    screen.text(fit(detail), 5, screen.height - 16)


def show(draw, *args):
    previous = image(screen.width, screen.height, display.surface(1))
    previous.blit(screen, vec2(0, 0))
    draw(*args)
    fade_start = time.ticks_ms()
    while (elapsed := time.ticks_diff(time.ticks_ms(), fade_start)) < FADE_MS:
        display.update(int((1 - easeInOutSine(elapsed / FADE_MS)) * 255))
    display.update()


show(draw_setup)

# put board into access point mode
logging.info("> going into access point mode")
ap = access_point("Art Matrix Setup", password="")
logging.info("  -", ap.ifconfig()[0])

# dns server to catch all dns requests
logging.info("> starting dns server...")
from phew import dns   # noqa E402
dns.run_catchall(ap.ifconfig()[0])

logging.info("> creating web server...")


def file_exists(filename):
    try:
        return (os.stat(filename)[0] & 0x4000) == 0
    except OSError:
        return False


def copy_file(source, target):
    with open(source, "rb") as infile:
        with open(target, "wb") as outfile:
            while True:
                chunk = infile.read(1024)
                if not chunk:
                    break
                outfile.write(chunk)


# create fresh config file if missing
if not file_exists("config.py"):
    copy_file("/defaults/config.py", "config.py")


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


@server.route("/provision-welcome", methods=["GET", "POST"])
def provision_welcome(request):
    if request.method == "POST":
        ssid = request.form.get("wifi_ssid", "")
        password = request.form.get("wifi_password", "")
        hostname = request.form.get("wifi_hostname", "")
        if ssid and password and hostname:
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
    global wlan, test_state, test_ssid, test_password, test_hostname, test_time_zone, test_started
    if test_state == "CONNECTING":
        return "OK"

    test_ssid = request.form.get("wifi_ssid", "")
    test_password = request.form.get("wifi_password", "")
    test_hostname = request.form.get("wifi_hostname", "")
    test_time_zone = request.form.get("time_zone", "")
    if not test_ssid:
        return "FAILED"

    show(draw_message, "Testing WiFi", test_ssid)

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


async def watch_test():
    global test_state
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
            show(draw_message, "Connected!", "Restarting...")
            await asyncio.sleep_ms(RESULT_HOLD_MS)
            machine.reset()

        if status < 0 or time.ticks_diff(time.ticks_ms(), test_started) > TEST_DEADLINE_MS:
            test_state = "FAILED"
            wlan.disconnect()
            show(draw_message, "Couldn't connect", test_ssid)
            await asyncio.sleep_ms(RESULT_HOLD_MS)
            if test_state == "FAILED":
                show(draw_setup)


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


# wait for a client to connect
logging.info("> waiting for a client to connect")
while len(ap.status("stations")) == 0:
    time.sleep(0.01)
logging.info("  - client connected!", ap.status("stations")[0])

logging.info("> running provisioning application...")
server.loop.create_task(watch_test())
server.run(host="0.0.0.0", port=80)
