needs_provisioning = False

try:
    import config   # fails to import (missing/corrupt) go into provisioning
    if not config.provisioned:   # provisioned flag not set go into provisioning
        needs_provisioning = True
except Exception:
    needs_provisioning = True

if needs_provisioning:
    import provisioning   # noqa F401,E402
    # control never returns to here, provisioning takes over completely

import wifi   # noqa E402

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

print(wifi.ipv4())

wifi.disconnect()
