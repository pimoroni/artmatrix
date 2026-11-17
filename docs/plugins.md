# Plugins

## Files

Each plugin is a folder in `/lib/plugins/`, named in lowercase with underscores (e.g. `my_plugin`).

| File | |
|---|---|
| `__init__.py` | The plugin. |
| `package.json` | Name, description, author, category, `requires_internet`. |
| `thumbnail.png` | 128×128 picture for the install and settings pages. |
| `config.html` | Optional settings page. |
| `config.json` | Saved settings, written by Art Matrix. |

## Your First Plugin

```python
BLACK = color.rgb(0, 0, 0)
RED = color.rgb(255, 0, 0)

x = 0


def update():
    global x
    screen.pen = BLACK
    screen.clear()
    screen.pen = RED
    screen.circle(x, screen.height // 2, 10)
    x = (x + 1) % screen.width
```

To try it:

1. Connect your Art Matrix to [Thonny](https://thonny.org) over USB.
2. In Thonny's Files pane, open `/lib/plugins/` on the Art Matrix, then right-click your `my_plugin` folder and choose Upload.
3. Press RESET on your Art Matrix.
4. Turn your plugin on from the settings page at `http://<name>.local`.

## How It Runs

* Art Matrix imports your plugin when it starts up, or when you turn the plugin on. This runs the top level of `__init__.py` once, so load images and fonts and build shapes there. The plugin then stays loaded until it's turned off.
* While your plugin is showing, Art Matrix calls `update()` every frame. Draw one frame into `screen` and return. Art Matrix shows the frame itself, so there's no need to call `display.update()`.
* `screen` isn't cleared between frames. Clear it, or draw over every pixel.
* Anything you set on `screen`, such as `pen`, `font`, `antialias`, `fill_rule`, `alpha` or `clip`, stays as your plugin left it, even after other plugins have run.
* `screen` is a different image each time your plugin comes round, so that Art Matrix can fade between plugins. Always draw to `screen` itself, never a copy kept in a variable.
* If `update()` raises an exception, that frame is skipped. After 5 in a row, Art Matrix moves on to the next plugin. If the import fails, your plugin is turned off.
* A plugin with `"requires_internet": true` only shows while Art Matrix is online.
* To split a plugin into several files, put them in its folder and import them with `from plugins.my_plugin import helper`.

## Settings

In `__init__.py`:

```python
from plugin_config import plugin_config

cfg = plugin_config("my_plugin", {
    "speed": 1.0,
    "colour": (255, 0, 0),
})
speed = cfg["speed"]
colour = cfg["colour"]
```

`config.html`:

```html
<form method="post" action="/config?plugin={{ plugin }}" data-autosave>
  <fieldset>
    <legend>Speed</legend>
    <div class="presets">
      <label><input type="radio" name="speed" value="0.5" {{ "checked" if placeholder.speed == 0.5 else "" }}>Slow</label>
      <label><input type="radio" name="speed" value="1.0" {{ "checked" if placeholder.speed == 1.0 else "" }}>Normal</label>
      <label><input type="radio" name="speed" value="2.0" {{ "checked" if placeholder.speed == 2.0 else "" }}>Fast</label>
    </div>
  </fieldset>

  <fieldset>
    <legend>Colour</legend>
    <input type="color" name="colour" aria-label="Colour" value="{{ '#%02x%02x%02x' % placeholder.colour }}">
  </fieldset>
</form>
```

| | |
|---|---|
| `plugin_config(name, defaults)` | Returns your plugin's saved settings as a dict. Each value has the same type as its default: an `int`, a `float`, an `(r, g, b)` colour, or a `bool`, which is `True` if the checkbox was ticked. A missing or invalid value gives the default. |
| `{{ plugin }}` | Your plugin's folder name. |
| `name="speed"` | Each field's `name` matches a key in the defaults. |
| `data-autosave` | Saves the form whenever a field changes, as the page has no Save button. |
| `placeholder` | Your plugin's module, so `placeholder.speed` is the value it's running with. |

When the form is saved, the settings are written to `config.json` and the plugin is imported again. A Reset button is added below your form, which deletes `config.json`. `config.html` is a phew template, and `{{ name }}` gives your plugin's display name.

To handle the form yourself, define `process_config(form)`. It's called with the form as a dict instead of writing `config.json`, and the page then has no Reset button.

## package.json

```json
{
  "name": "Stars",
  "description": "Flying through a starfield",
  "author": "Pimoroni",
  "category": "Visualisations",
  "requires_internet": false
}
```

| | |
|---|---|
| `name` | The name shown on the install and settings pages. |
| `description` | A short description, 60 characters at most. |
| `author` | Who made the plugin. |
| `category` | `Information`, `Visualisations`, `Scenes`, `Interactive` or `3D`. |
| `requires_internet` | `true` to show it only while Art Matrix is online. |

To add a plugin to the install list or contribute it, see the [plugins repository](https://github.com/pimoroni/artmatrix-plugins#adding-it-to-the-list).
