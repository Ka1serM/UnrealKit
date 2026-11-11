import argparse
import json
import os
import pathlib
import shutil
import subprocess
import sys
import time

SKIPPED = 77
EXPORT_TIMEOUT = 1800
START_TIMEOUT = 120


def fail(message):
    print(message, file=sys.stderr)
    sys.exit(1)


def make_resources(work, extension, extension_id):
    resources = work / "blender"
    shutil.rmtree(resources, ignore_errors=True)
    repository = resources / "extensions" / "user_default"
    repository.mkdir(parents=True)
    link = repository / extension_id
    if os.name == "nt":
        import _winapi
        _winapi.CreateJunction(str(extension), str(link))
    else:
        link.symlink_to(extension, target_is_directory=True)
    return resources


def stop(process):
    if process.poll() is None:
        process.kill()
        process.wait()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--blender", required=True)
    parser.add_argument("--extension", required=True, type=pathlib.Path)
    parser.add_argument("--extension-id", required=True)
    parser.add_argument("--work", required=True, type=pathlib.Path)
    parser.add_argument("--animated", action="store_true", help="the scene must have animated objects")
    parser.add_argument("driver", nargs=argparse.REMAINDER)
    arguments = parser.parse_args()
    driver = arguments.driver[1:] if arguments.driver[:1] == ["--"] else arguments.driver
    if not driver:
        fail("Give the driver command after --.")

    work = arguments.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    ready, result_path, blend = work / "ready", work / "result.json", work / "scene.blend"
    for stale in (ready, result_path, blend):
        stale.unlink(missing_ok=True)
    environment = dict(os.environ, BLENDER_USER_RESOURCES=str(make_resources(work, arguments.extension.resolve(), arguments.extension_id)))
    receive = pathlib.Path(__file__).with_name("blender_receive.py")

    with open(work / "blender.log", "w") as blender_log, open(work / "driver.log", "w") as driver_log:
        blender = subprocess.Popen(
            [arguments.blender, "-b", "--factory-startup", "--python-exit-code", "1", "--python", str(receive), "--",
             arguments.extension_id, str(ready), str(result_path), str(blend), str(EXPORT_TIMEOUT)],
            stdout=blender_log, stderr=subprocess.STDOUT, env=environment)
        try:
            deadline = time.monotonic() + START_TIMEOUT
            while not ready.exists():
                if blender.poll() is not None or time.monotonic() > deadline:
                    break
                time.sleep(0.05)
            if not ready.exists():
                stop(blender)
                state = json.loads(result_path.read_text())["state"] if result_path.exists() else "Blender did not start"
                fail(f"{state}. See {work / 'blender.log'}.")

            started = time.monotonic()
            code = subprocess.call(driver, stdout=driver_log, stderr=subprocess.STDOUT)
            print(f"The driver exited with {code} after {time.monotonic() - started:.1f} s.")
            if code == SKIPPED:
                print("The driver has no game to read: set the game in the app's settings.")
                return SKIPPED
            if code != 0:
                fail(f"The driver failed. See {work / 'driver.log'}.")
            blender.wait(timeout=EXPORT_TIMEOUT)
        finally:
            stop(blender)

    if not result_path.exists():
        fail(f"Blender wrote no result. See {work / 'blender.log'}.")
    result = json.loads(result_path.read_text())
    print(json.dumps({key: value for key, value in result.items() if key != "diagnostics"}))
    for diagnostic in result.get("diagnostics", []):
        print("Blender:", *diagnostic[1:])
    if result["state"] != "Finished":
        fail(f"Blender did not finish the export: {result['state']}.")
    if result["objects"] == 0:
        fail("The scene has no objects.")
    if arguments.animated and result["animated"] == 0:
        fail("The scene has no animated objects.")
    return 0


sys.exit(main())
