#!/usr/bin/env python3
"""Exercise native extension popup and settings UI in actual Gnosis windows."""

import argparse
import os
from pathlib import Path
import resource
import shutil
import signal
import subprocess
import tempfile
import time


resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("binary", type=Path)
parser.add_argument("--schemas", type=Path, required=True)
parser.add_argument("--ublock", type=Path, default=os.environ.get("UBLOCK_DIR"))
parser.add_argument("--extension-x-offset", type=int, default=262,
                    help="Extension button distance from the window's right edge")
parser.add_argument("--settings-x-offset", type=int, default=136,
                    help="Settings button distance from the window's right edge")
parser.add_argument("--row-y-offset", type=int, default=78,
                    help="Extension row distance from the window's top edge")
args = parser.parse_args()

if not args.ublock or not (args.ublock / "manifest.json").is_file():
    parser.error("Set UBLOCK_DIR or --ublock to the official unpacked full uBlock Origin")
if "GNOSIS_TEST_URL" not in os.environ:
    parser.error("GNOSIS_TEST_URL must point at the check-native fixture server")

artifacts = Path(tempfile.mkdtemp(prefix="gnosis-native-extensions-ui-", dir="/tmp"))
profile = artifacts / "profile"
env = dict(os.environ,
           LANG="C.UTF-8",
           XDG_CONFIG_HOME=str(artifacts / "config"),
           XDG_DATA_HOME=str(artifacts / "data"),
           XDG_CACHE_HOME=str(artifacts / "cache"),
           GSETTINGS_BACKEND="keyfile",
           GSETTINGS_SCHEMA_DIR=str(args.schemas.resolve()),
           GNOSIS_UBLOCK_DIR=str(args.ublock.resolve()))
env.pop("WAYLAND_DISPLAY", None)

subprocess.run(["gsettings", "set", "com.neg-zero.Gnosis", "ask-for-default", "false"],
               env=env, check=True)


def xdo(*arguments, capture=False):
    command = ["xdotool", *map(str, arguments)]
    if capture:
        return subprocess.check_output(command, env=env, text=True).strip()
    subprocess.run(command, env=env, check=True)


def wait_for(predicate, message, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError((artifacts / "app.log").read_text())
        result = predicate()
        if result:
            return result
        time.sleep(.1)
    raise TimeoutError(message + "\n" + (artifacts / "app.log").read_text())


def windows():
    result = subprocess.run(["xdotool", "search", "--onlyvisible", "--pid", str(process.pid)],
                            env=env, capture_output=True, text=True)
    return result.stdout.splitlines() if result.returncode == 0 else []


def app_windows():
    return [window for window in windows()
            if geometry(window).get("WIDTH", 0) >= 600 and geometry(window).get("HEIGHT", 0) >= 400]


def geometry(window):
    values = {}
    for line in xdo("getwindowgeometry", "--shell", window, capture=True).splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            if key in {"X", "Y", "WIDTH", "HEIGHT"}:
                values[key] = int(value)
    return values


def keys(*values):
    xdo("key", "--clearmodifiers", "--delay", "80", *values)


def navigate(uri):
    keys("ctrl+l")
    xdo("type", "--clearmodifiers", "--delay", "0", uri)
    keys("Return")


def screenshot(name, required=True):
    result = subprocess.run(["magick", "import", "-window", "root", str(artifacts / name)], env=env)
    if required:
        result.check_returncode()


def wait_for_popup_content(window):
    box = geometry(window)
    probe = artifacts / "popup-probe.png"
    crop = f"260x180+{box['X'] + box['WIDTH'] - 420}+{box['Y'] + 300}"

    def rendered():
        screenshot(probe.name)
        deviation = subprocess.check_output([
            "magick", str(probe), "-crop", crop, "-colorspace", "gray",
            "-format", "%[fx:standard_deviation]", "info:",
        ], env=env, text=True)
        if float(deviation) <= .03:
            return False
        shutil.copyfile(probe, artifacts / "ublock-popup.png")
        return True

    return wait_for(rendered, "The uBlock popup stayed visually blank", timeout=30)


def wait_for_dashboard_content(window):
    box = geometry(window)
    probe = artifacts / "dashboard-probe.png"
    crop = f"{box['WIDTH'] - 40}x{box['HEIGHT'] - 180}+{box['X'] + 20}+{box['Y'] + 130}"

    def rendered():
        if "Dashboard" not in xdo("getwindowname", window, capture=True):
            return False
        screenshot(probe.name)
        deviation = subprocess.check_output([
            "magick", str(probe), "-crop", crop, "-colorspace", "gray",
            "-format", "%[fx:standard_deviation]", "info:",
        ], env=env, text=True)
        if float(deviation) <= .02:
            return False
        shutil.copyfile(probe, artifacts / "ublock-dashboard.png")
        return True

    return wait_for(rendered, "The chrome-extension dashboard DOM stayed blank", timeout=30)


def click_extension_button(window):
    box = geometry(window)
    xdo("mousemove", box["X"] + box["WIDTH"] - args.extension_x_offset,
        box["Y"] + 24, "click", "1")


def click_extension_row(window, settings=False):
    box = geometry(window)
    x_offset = args.settings_x_offset if settings else 300
    xdo("mousemove", box["X"] + box["WIDTH"] - x_offset,
        box["Y"] + args.row_y_offset, "click", "1")


log = (artifacts / "app.log").open("w")
process = subprocess.Popen([str(args.binary.resolve()), "--profile=" + str(profile),
                            os.environ["GNOSIS_TEST_URL"] + "session-fixture?first"],
                           env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)

try:
    first = wait_for(lambda: app_windows()[0] if app_windows() else None, "Main window did not appear")
    xdo("windowsize", first, 960, 700)
    xdo("windowmove", first, 20, 20)
    xdo("windowfocus", "--sync", first)
    wait_for(lambda: "Saved tab first" in xdo("getwindowname", first, capture=True),
             "First owner page did not load")

    keys("ctrl+n")
    second = wait_for(lambda: next((item for item in app_windows() if item != first), None),
                      "Second window did not appear")
    xdo("windowsize", second, 960, 700)
    xdo("windowmove", second, 20, 20)
    xdo("windowfocus", "--sync", second)
    second_base = os.environ["GNOSIS_TEST_URL"].replace("127.0.0.1", "localhost")
    navigate(second_base + "session-fixture?second")
    wait_for(lambda: "Saved tab second" in xdo("getwindowname", second, capture=True),
             "Second owner page did not load")
    first_title = xdo("getwindowname", first, capture=True)
    keys("Escape")
    time.sleep(.25)
    screenshot("owner-window.png")

    click_extension_button(second)
    time.sleep(.5)
    screenshot("extensions-menu.png")
    click_extension_row(second)
    wait_for_popup_content(second)
    assert set(app_windows()) == {first, second}, "Extension popup escaped into a separate top-level window"

    keys("Escape")
    time.sleep(.25)
    click_extension_button(second)
    time.sleep(.35)
    screenshot("settings-menu.png")
    click_extension_row(second, settings=True)
    wait_for_dashboard_content(second)
    assert xdo("getwindowname", first, capture=True) == first_title, \
        "Opening settings changed the other Gnosis window"
    assert set(app_windows()) == {first, second}, "Extension settings escaped into a separate top-level window"

    print("Native Extensions menu, embedded uBlock popup, and owning-window dashboard: PASS", flush=True)
    keys("ctrl+q")
    assert process.wait(timeout=10) == 0, (artifacts / "app.log").read_text()
except BaseException:
    screenshot("failure.png", required=False)
    raise
finally:
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    log.close()
    print("Native extension UI artifacts:", artifacts, flush=True)
