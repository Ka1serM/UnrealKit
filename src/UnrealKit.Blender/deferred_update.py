import bpy
from bpy.app.handlers import persistent

from .product import ID, NAME


class DeferredUpdateTree(bpy.types.NodeTree):
    bl_idname = f"{ID.upper()}_DeferredUpdateTree"
    bl_label = f"{NAME} Deferred Update"
    bl_icon = "NODETREE"

    @classmethod
    def poll(cls, context):
        return False

    def update(self):
        _run_pending()


_pending = None
_error = None
_trigger = None


def _run_pending():
    global _pending, _error
    edits, _pending = _pending, None
    if edits is None:
        return
    try:
        edits()
    except BaseException as error:
        _error = error


def run(edits, tree):
    global _pending, _error, _trigger
    _pending, _error = edits, None
    if _trigger is None:
        _trigger = bpy.data.node_groups.new(DeferredUpdateTree.bl_label, DeferredUpdateTree.bl_idname)
    _trigger.nodes.remove(_trigger.nodes.new("NodeReroute"))
    if _pending is not None:
        _pending = None
        raise RuntimeError("the deferred node edits did not run: a node tree update was already running")
    error, _error = _error, None
    if error is not None:
        raise error
    if tree.nodes:
        tree.nodes[0].update()


@persistent
def _forget_trigger(*_):
    global _trigger
    _trigger = None


def register():
    bpy.utils.register_class(DeferredUpdateTree)
    bpy.app.handlers.load_post.append(_forget_trigger)


def unregister():
    global _trigger
    bpy.app.handlers.load_post.remove(_forget_trigger)
    if _trigger is not None:
        bpy.data.node_groups.remove(_trigger)
        _trigger = None
    bpy.utils.unregister_class(DeferredUpdateTree)
