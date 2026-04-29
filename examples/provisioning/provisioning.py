import os
import json
import time
import machine
from lib.phew import logging, server, redirect, serve_file, render_template, access_point
from machine import unique_id
import qrcode
from binascii import hexlify

screen.font = rom_font.nope

code = qrcode.QRCode()

DOMAIN = "pico.wireless"
ssid = "Art Matrix Setup"
password = hexlify(unique_id()).decode('utf-8')[:8]
pw_length, _ = screen.measure_text(password)
ssid_length, _ = screen.measure_text(ssid)

# create the qr payload
qr_payload = f"WIFI:T:WPA;S:{ssid};P:{password};;"

# put board into access point mode
logging.info("> going into access point mode")
ap = access_point("Art Matrix Setup", password=password)
logging.info("  -", ap.ifconfig()[0])

# dns server to catch all dns requests
logging.info("> starting dns server...")
from lib.phew import dns   # noqa E402
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
    copy_file("config_template.py", "config.py")


# write the current values in config to the config.py file
def write_config():
    lines = []
    with open("config.py", "r") as infile:
        lines = infile.read().split("\n")

    for i in range(0, len(lines)):
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


def measure_qr_code(size, code):
    w, h = code.get_size()
    module_size = int(size / w)
    return module_size * w, module_size


def draw_qr_code(ox, oy, size, code):
    size, module_size = measure_qr_code(size, code)
    ox = (screen.width / 2) - (size / 2)
    screen.pen = color.rgb(0, 0, 0)
    screen.rectangle(ox, oy, size, size)
    screen.pen = color.white
    for x in range(size):
        for y in range(size):
            if code.get_module(x, y):
                screen.rectangle(ox + x * module_size, oy + y * module_size, module_size, module_size)

    tx = 64 - (ssid_length / 2)
    screen.text(f"{ssid}", tx, y + 12)
    tx = 64 - (pw_length / 2)
    screen.text(f"{password}", tx, y + 24)


screen.pen = color.rgb(0, 0, 0)
screen.clear()
code.set_text(qr_payload)
draw_qr_code(0, 4, 110, code)
display.update()


# TODO This did not seem to work for me...
@server.route("/wrong-host-redirect", methods=["GET"])
def wrong_host_redirect(request):
    # if the client requested a resource at the wrong host then present
    # a meta redirect so that the captive portal browser can be sent to the correct location
    body = f"<!DOCTYPE html><head><meta http-equiv=\"refresh\" content=\"0;URL='http://{DOMAIN}/provision-welcome'\" /></head>"
    return body


@server.route("/provision-welcome", methods=["GET"])
def provision_welcome(request):
    response = render_template("html/welcome.html")
    return response


@server.route("/provision-step-1-nickname", methods=["GET", "POST"])
def provision_step_1_nickname(request):
    if request.method == "POST":
        config.nickname = request.form["nickname"]
        write_config()
        return redirect(f"http://{DOMAIN}/provision-step-2-wifi")
    else:
        return render_template("html/provision-step-1-nickname.html")


@server.route("/provision-step-2-wifi", methods=["GET", "POST"])
def provision_step_2_wifi(request):
    if request.method == "POST":
        config.wifi_ssid = request.form["wifi_ssid"]
        config.wifi_password = request.form["wifi_password"]
        write_config()
        return redirect(f"http://{DOMAIN}/provision-step-5-done")
    else:
        return render_template("html/provision-step-2-wifi.html")


@server.route("/provision-step-5-done", methods=["GET", "POST"])
def provision_step_5_done(request):
    config.provisioned = True
    write_config()

    # a post request to the done handler means we're finished and
    # should reset the board
    if request.method == "POST":
        machine.reset()
        return

    return render_template("html/provision-step-5-done.html")


@server.route("/networks.json")
def networks(request):
    networks = []
    for network in ap.scan():
        network = network[0].decode("ascii").strip()
        if network != "":
            networks.append(network)
    networks = list(set(networks))   # remove duplicates
    return json.dumps(networks), 200, "application/json"


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


# wait for a client to connect
logging.info("> waiting for a client to connect")
while len(ap.status("stations")) == 0:
    time.sleep(0.01)
logging.info("  - client connected!", ap.status("stations")[0])

logging.info("> running provisioning application...")
server.run(host="0.0.0.0", port=80)
