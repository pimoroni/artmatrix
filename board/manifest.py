require("bundle-networking")
require("urllib.urequest")
require("umqtt.simple")
require("bisect")

# Bluetooth
require("aioble")

include("$(PORT_DIR)/boards/manifest.py")

# Handy for dealing with APIs
require("datetime")

freeze("../modules/python/")
