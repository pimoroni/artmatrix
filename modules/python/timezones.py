# local time for the zone picked on the settings page. the rtc stays on utc, so
# plugins showing wall-clock time call timezones.localtime() rather than
# time.localtime()

import time

import config

# daylight saving rules: when the clocks go forward, then back, each as
# (month, week, weekday, hour), then whether those hours are utc. week 0 is the
# month's last, 1-4 count from its start. weekday 0 is monday. hours are local
# standard time, so a change at 24:00 daylight time is 23:00 here
RULES = {
    "eu": ((3, 0, 6, 1), (10, 0, 6, 1), True),
    "us": ((3, 2, 6, 2), (11, 1, 6, 1), False),
    "australia": ((10, 1, 6, 2), (4, 1, 6, 2), False),
    "nz": ((9, 0, 6, 2), (4, 1, 6, 2), False),
    "egypt": ((4, 0, 4, 0), (10, 0, 3, 23), False),
    "cuba": ((3, 2, 6, 0), (11, 1, 6, 0), False),
}

# (key, name, minutes ahead of utc in standard time, daylight saving rule), west to east
ZONES = (
    ("american_samoa", "American Samoa", -660, None),
    ("hawaii", "Hawaii", -600, None),
    ("alaska", "Alaska", -540, "us"),
    ("pacific", "US & Canada Pacific", -480, "us"),
    ("arizona", "Arizona", -420, None),
    ("mountain", "US & Canada Mountain", -420, "us"),
    ("mexico", "Mexico City", -360, None),
    ("central", "US & Canada Central", -360, "us"),
    ("colombia", "Colombia, Peru & Panama", -300, None),
    ("cuba", "Cuba", -300, "cuba"),
    ("eastern", "US & Canada Eastern", -300, "us"),
    ("venezuela", "Venezuela, Bolivia & Puerto Rico", -240, None),
    ("atlantic", "Atlantic Canada", -240, "us"),
    ("newfoundland", "Newfoundland", -210, "us"),
    ("brazil", "Brazil & Argentina", -180, None),
    ("cape_verde", "Cape Verde", -60, None),
    ("azores", "Azores", -60, "eu"),
    ("utc", "UTC", 0, None),
    ("uk", "UK, Ireland & Portugal", 0, "eu"),
    ("west_africa", "West Africa", 60, None),
    ("central_europe", "Central Europe", 60, "eu"),
    ("eastern_europe", "Eastern Europe", 120, "eu"),
    ("egypt", "Egypt", 120, "egypt"),
    ("south_africa", "South Africa", 120, None),
    ("moscow", "Moscow, Turkey & Saudi Arabia", 180, None),
    ("iran", "Iran", 210, None),
    ("gulf", "UAE & Oman", 240, None),
    ("pakistan", "Pakistan", 300, None),
    ("india", "India & Sri Lanka", 330, None),
    ("nepal", "Nepal", 345, None),
    ("bangladesh", "Bangladesh", 360, None),
    ("indochina", "Thailand, Vietnam & Jakarta", 420, None),
    ("china", "China, Singapore & Perth", 480, None),
    ("japan", "Japan & Korea", 540, None),
    ("darwin", "Darwin", 570, None),
    ("adelaide", "Adelaide", 570, "australia"),
    ("brisbane", "Brisbane", 600, None),
    ("sydney", "Sydney, Melbourne & Hobart", 600, "australia"),
    ("solomon", "Solomon Islands & New Caledonia", 660, None),
    ("fiji", "Fiji", 720, None),
    ("nz", "New Zealand", 720, "nz"),
    ("samoa_tonga", "Samoa & Tonga", 780, None),
    ("line_islands", "Kiribati Line Islands", 840, None),
)

# the zone and utc year the changeovers below were worked out for
cached = None
offset = 0
forward = None
back = None


def zone():
    for row in ZONES:
        if row[0] == config.time_zone:
            return row
    raise ValueError(f"unknown time zone {config.time_zone}")


# "UTC+5:30" for 330 minutes ahead of utc
def offset_label(minutes):
    hours, rest = divmod(abs(minutes), 60)
    sign = "-" if minutes < 0 else "+"
    return f"UTC{sign}{hours}:{rest:02d}" if rest else f"UTC{sign}{hours}"


# (key, label) for each zone, for the pages' selects. labels give the standard time offset
def choices():
    pairs = []
    for key, name, minutes, _ in ZONES:
        # utc itself would only repeat its name
        if name != "UTC":
            name += f" ({offset_label(minutes)})"
        pairs.append((key, name))
    return pairs


# seconds since the epoch the clocks change at, from one half of a rule
def changeover(year, when, minutes, in_utc):
    month, week, weekday, hour = when
    if week:
        first = time.mktime((year, month, 1, 0, 0, 0, 0, 0))
        day = first + ((weekday - time.gmtime(first)[6]) % 7 + (week - 1) * 7) * 86400
    else:
        # the month's last day is the day before the next month's first
        last = time.mktime((year + month // 12, month % 12 + 1, 1, 0, 0, 0, 0, 0)) - 86400
        day = last - (time.gmtime(last)[6] - weekday) % 7 * 86400
    return day + hour * 3600 - (0 if in_utc else minutes * 60)


# minutes ahead of utc at now, seconds since the epoch, daylight saving included
def utc_offset(now):
    global cached, offset, forward, back

    year = time.gmtime(now)[0]

    if cached != (config.time_zone, year):
        # cached last, so an unknown zone raises every time rather than once
        _, _, offset, rule = zone()
        forward = back = None
        if rule:
            start, end, in_utc = RULES[rule]
            forward = changeover(year, start, offset, in_utc)
            back = changeover(year, end, offset, in_utc)
        cached = (config.time_zone, year)

    if forward is None:
        return offset

    # southern zones go forward late in the year and back early in it
    if forward < back:
        saving = forward <= now < back
    else:
        saving = now >= forward or now < back
    return offset + 60 if saving else offset


def localtime():
    now = time.time()
    return time.gmtime(now + utc_offset(now) * 60)
