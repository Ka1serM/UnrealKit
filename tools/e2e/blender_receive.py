import importlib
import json
import pathlib
import sys
import time

import bpy

extension_id, ready, result_path, blend, timeout = sys.argv[sys.argv.index("--") + 1:]
deadline = time.monotonic() + float(timeout)


def write_result(**result):
    pathlib.Path(result_path).write_text(json.dumps(result, indent=2))


module = f"bl_ext.user_default.{extension_id}"
bpy.ops.preferences.addon_enable(module=module)
extension = importlib.import_module(module)
while extension.server.listener is None and extension.server.error is None and time.monotonic() < deadline:
    time.sleep(0.01)
if extension.server.error is not None or extension.server.listener is None:
    write_result(state=f"Cannot listen: {extension.server.error}")
    sys.exit(0)
time.sleep(0.2)
pathlib.Path(ready).touch()

while extension.last_result is None and time.monotonic() < deadline:
    extension.import_handler()
    time.sleep(0.005)
if extension.last_result is None:
    write_result(state="No export arrived")
    sys.exit(0)

title, state, diagnostics = extension.last_result
scene = bpy.context.scene
write_result(
    title=title,
    state=state,
    diagnostics=[list(diagnostic) for diagnostic in diagnostics],
    objects=len(bpy.data.objects),
    animated=sum(1 for item in bpy.data.objects if item.animation_data and item.animation_data.action),
    frame_start=scene.frame_start,
    frame_end=scene.frame_end,
)
bpy.ops.wm.save_as_mainfile(filepath=blend)
bpy.ops.preferences.addon_disable(module=module)
