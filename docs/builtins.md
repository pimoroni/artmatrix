# Builtins

Available to every plugin, with no import.

## screen

The 128×128 `image` your plugin draws into. It's a different image each time your plugin comes round, so always use the name `screen`. See [How It Runs](plugins.md#how-it-runs).

## display

| | |
|---|---|
| `display.scratch()` | Returns the pixels of a spare 128×128 image in fast on-chip memory. Wrap it with `image(128, 128, display.scratch())`. Every plugin shares it, and another plugin can draw into it while yours isn't showing or during a fade. Draw into it and use it within a single `update()`. |
| `display.update(blend=0)` | Shows the current frame. `blend`, from 0 to 255, mixes surface 0 into surface 1. Does nothing while `set_hold(True)` is in effect. |
| `display.surface(n)` | Returns the pixels of drawing surface 0 or 1. |
| `display.set_hold(hold)` | While `hold` is `True`, `update()` does nothing. Returns how many `update()` calls were ignored since the last `set_hold()`. |
| `display.set_blocking(blocking)` | Sets whether `update()` waits for the frame to finish copying. Defaults to `True`. |
| `display.is_busy()` | Returns `True` while a frame is still being copied. |
| `display.set_brightness(value)` | Sets the brightness, from 0 to 1, on a scale that looks even to the eye. The panel fades to it. |
| `display.set_ambient(on)` | Turns the rear LEDs on or off. |
| `display.set_current_limit(amps)` | Sets the most current the panel can draw, up to 3.0 A. |
| `display.force_on(on=True)` | Lets the panel go brighter than its measured limit. `force_on(False)` turns this off. The brownout protection still applies. |
| `display.calibrate()` | Throws away the stored power measurement and measures again. |
| `display.calibrated()` | Returns `True` once the power measurement has finished. |
| `display.power_stats()` | Returns `(vled_open, vled_mean, vled_min, cur, cur_peak, cur_idle, calibrating, headroom_v, headroom_i, ceiling, allowed, applied, gain_i, gain_v)`. VLED values are 1.61 mV a count, and CUR values are 8.46 mA a count. |
| `display.sensor()` | Returns the SENS port's voltage, averaged over 4 ms, from 0 to 65535. The same as `sensor.read_u16()`. |

Art Matrix uses `update`, `surface`, `set_hold`, `set_blocking` and `is_busy` to run plugins. `set_brightness`, `set_ambient`, `set_current_limit`, `force_on` and `calibrate` override the owner's settings and the panel's power limits.

## Buttons

| | |
|---|---|
| `BUTTON_A`, `BUTTON_B`, `BUTTON_C`, `BUTTON_USER` | The four button pins. A pressed button reads 0. Use them with `machine.Pin(BUTTON_A, Pin.IN, Pin.PULL_UP)`, except `BUTTON_USER`, which takes no pull: `machine.Pin(BUTTON_USER, Pin.IN)`. |

## sensor

| | |
|---|---|
| `sensor.read_u16()` | Returns the SENS port's voltage, averaged over 4 ms, from 0 to 65535. Use this rather than `machine.ADC`. |

## Helpers

For `clamp`, `rnd`, `frnd`, `file_exists`, `is_dir` and `free`, see the Badgeware [builtins](https://badgewa.re/docs/api/builtins.md) page.
