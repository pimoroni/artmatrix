import json


def plugin_config(name, defaults):
    try:
        with open(f"/lib/plugins/{name}/config.json") as f:
            saved = json.load(f)
    except (OSError, ValueError):
        return dict(defaults)

    cfg = {}
    for key, default in defaults.items():
        if isinstance(default, bool):
            cfg[key] = key in saved
            continue

        value = saved.get(key)
        try:
            if value in (None, ""):
                value = default
            elif isinstance(default, int):
                value = int(value)
            elif isinstance(default, float):
                value = float(value)
            elif isinstance(default, tuple):
                value = tuple(int(value[i:i + 2], 16) for i in (1, 3, 5))
        except ValueError:
            value = default
        cfg[key] = value
    return cfg
