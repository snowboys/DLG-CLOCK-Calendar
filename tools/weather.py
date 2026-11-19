"""weather.py - wttr.in 天气 (免费, 不需要 key)

返回: {city, desc, temp, hi, lo, hum, kind}
带 20 分钟本地缓存; 拿不到就用上一次的值, 再不行给占位数据。
"""
import json
import os
import time
import urllib.parse
import urllib.request

CACHE_PATH = os.path.join(os.environ.get("LOCALAPPDATA", os.path.expanduser("~")),
                          "pcmonitor", "weather.json")
TTL = 20 * 60
_last = {"city": "--", "desc": "NO DATA", "temp": "--", "hi": "--",
         "lo": "--", "hum": "--", "kind": 6, "ts": 0}
_last_city = None


def _wmo_kind(code):
    try:
        c = int(code)
    except Exception:
        return 2
    if c == 113:
        return 0
    if c in (116, 119, 122):
        return 1
    if c in (143, 248, 260):
        return 6
    if c in (176, 185, 263, 266, 281, 284, 293, 296, 299, 302, 305, 308, 311, 314, 353, 356, 359):
        return 3
    if c in (179, 182, 227, 230, 317, 320, 323, 326, 329, 332, 335, 338, 350, 368, 371, 374, 377):
        return 4
    if c in (200, 386, 389, 392, 395):
        return 5
    return 2


_ZH = {
    "sunny": "晴", "clear": "晴", "partly cloudy": "晴间多云", "cloudy": "多云",
    "overcast": "阴", "mist": "薄雾", "fog": "雾", "freezing fog": "冻雾",
    "haze": "霾", "smoky haze": "烟霾", "smog": "雾霾", "severe smog": "重度雾霾",
    "smoke": "烟霾", "patchy rain possible": "零星小雨",
    "patchy rain nearby": "零星小雨", "light rain": "小雨", "moderate rain": "中雨",
    "heavy rain": "大雨", "light drizzle": "毛毛雨", "patchy light drizzle": "零星毛毛雨",
    "light rain shower": "小阵雨", "patchy light rain": "零星小雨",
    "moderate or heavy rain shower": "阵雨", "torrential rain shower": "暴雨",
    "moderate or heavy rain in area with thunder": "雷阵雨",
    "patchy light rain in area with thunder": "雷阵雨",
    "patchy snow possible": "零星小雪", "light snow": "小雪", "moderate snow": "中雪",
    "heavy snow": "大雪", "blizzard": "暴雪", "patchy light snow": "零星小雪",
    "patchy moderate snow": "阵雪", "moderate or heavy snow showers": "阵雪",
    "light sleet": "小雨夹雪", "moderate or heavy sleet": "雨夹雪",
    "thunderstorm": "雷阵雨", "patchy light rain with thunder": "雷阵雨",
    "thundery outbreaks possible": "雷阵雨", "thundery outbreaks in nearby": "附近雷雨",
    "light freezing rain": "冻雨", "moderate or heavy freezing rain": "冻雨",
}


def _zh(s):
    return _ZH.get(str(s).strip().lower(), s)


def _pick(d, key, default=""):
    """wttr.in 的中文字段形如 lang_zh: [{"value": "多云"}]"""
    try:
        v = d[key]
        if isinstance(v, list):
            return v[0].get("value", default)
        return v
    except Exception:
        return default


def _fetch(city):
    url = "https://wttr.in/%s?format=j1&lang=zh" % urllib.parse.quote(city)
    req = urllib.request.Request(url, headers={"User-Agent": "curl/8.5.0"})
    with urllib.request.urlopen(req, timeout=8) as r:
        j = json.loads(r.read().decode("utf-8", "ignore"))

    cur = j["current_condition"][0]
    day = (j.get("weather") or [{}])[0]
    area = (j.get("nearest_area") or [{}])[0]

    desc = _zh(_pick(cur, "lang_zh") or _pick(cur, "weatherDesc"))
    name = _pick(area, "lang_zh") or _pick(area, "areaName") or city
    if any(ord(ch) > 127 for ch in city):
        name = city                    # 用户输入的是中文就用输入值

    return {
        "city": name,
        "desc": desc,
        "temp": int(cur.get("temp_C", 0)),
        "hi": int(day.get("maxtempC", 0) or 0),
        "lo": int(day.get("mintempC", 0) or 0),
        "hum": int(cur.get("humidity", 0) or 0),
        "kind": _wmo_kind(cur.get("weatherCode")),
        "ts": time.time(),
    }


def get_weather(city, ttl=TTL):
    """带缓存: 同城市 + 未过期就直接用缓存"""
    global _last, _last_city

    # 换城市时缓存失效
    if city != _last_city:
        _last_city = city
        _last = dict(_last, ts=0)

    if _last.get("ts") and time.time() - _last["ts"] < ttl:
        return _last

    try:
        wx = _fetch(city)
    except Exception as e:
        print("[weather] %s: %r" % (city, e))
        if _last.get("ts"):
            return _last
        return dict(_last, city=city, desc="NO DATA")

    _last = wx
    try:
        os.makedirs(os.path.dirname(CACHE_PATH), exist_ok=True)
        with open(CACHE_PATH, "w", encoding="utf-8") as f:
            json.dump(wx, f, ensure_ascii=False)
    except OSError:
        pass
    return wx
