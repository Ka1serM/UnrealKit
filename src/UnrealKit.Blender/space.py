import math

import numpy as np
from mathutils import Matrix, Quaternion, Vector

METRES_PER_CENTIMETRE = 0.01
MIRROR = np.array([1.0, -1.0, 1.0, 1.0])
VIEW_BASIS = Matrix(((0, 0, -1, 0), (-1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 0, 1)))
VIEW_ROTATION = VIEW_BASIS.to_quaternion()
SHORTEST_BONE_LENGTH = 1e-4
BONE_DISPLAY_LENGTH = 0.04


def matrix(values):
    rows = np.asarray(values, dtype=np.float64).reshape(4, 4) * np.outer(MIRROR, MIRROR)
    rows[:3, 3] *= METRES_PER_CENTIMETRE
    return Matrix(rows.tolist())


def positions(values):
    result = np.asarray(values, dtype=np.float32).reshape(-1, 3) * np.float32(METRES_PER_CENTIMETRE)
    result[:, 1] = -result[:, 1]
    return result


def directions(values):
    result = np.asarray(values, dtype=np.float32).reshape(-1, 3).copy()
    result[:, 1] = -result[:, 1]
    result /= np.maximum(np.linalg.norm(result, axis=1, keepdims=True), np.float32(1e-8))
    return result


def quaternions(values):
    q = np.asarray(values, dtype=np.float64).reshape(-1, 4)
    return np.stack((q[:, 3], -q[:, 0], q[:, 1], -q[:, 2]), axis=1)


def transform(location, rotation, scale):
    return Matrix.LocRotScale(Vector(positions(location)[0].tolist()), Quaternion(quaternions(rotation)[0].tolist()),
                              Vector([float(value) for value in scale]))


def multiply(a, b):
    a, b = np.broadcast_arrays(np.atleast_2d(a), np.atleast_2d(b))
    return np.stack((a[:, 0] * b[:, 0] - a[:, 1] * b[:, 1] - a[:, 2] * b[:, 2] - a[:, 3] * b[:, 3],
                     a[:, 0] * b[:, 1] + a[:, 1] * b[:, 0] + a[:, 2] * b[:, 3] - a[:, 3] * b[:, 2],
                     a[:, 0] * b[:, 2] - a[:, 1] * b[:, 3] + a[:, 2] * b[:, 0] + a[:, 3] * b[:, 1],
                     a[:, 0] * b[:, 3] + a[:, 1] * b[:, 2] - a[:, 2] * b[:, 1] + a[:, 3] * b[:, 0]), axis=1)


def without_scale(m):
    result = m.copy()
    result[:3, :3] /= np.linalg.norm(m[:3, :3], axis=0)
    return result


def rigid_inverse(m):
    result = np.identity(4)
    result[:3, :3] = m[:3, :3].T
    result[:3, 3] = -result[:3, :3] @ m[:3, 3]
    return result


def axis_change_towards(d):
    ax, ay, az = abs(d[0]), abs(d[1]), abs(d[2])
    if ax >= ay and ax >= az:
        s = -1.0 if d[0] < 0 else 1.0
        return np.array(((0, s, 0), (-s, 0, 0), (0, 0, 1.0))), (1, 0, 2)
    if ay >= az:
        return np.diag((1.0, 1.0, 1.0) if d[1] > 0 else (-1.0, -1.0, 1.0)), (0, 1, 2)
    s = -1.0 if d[2] < 0 else 1.0
    return np.array(((1.0, 0, 0), (0, 0, -s), (0, s, 0))), (0, 2, 1)


def turn_from_y(d):
    x, z = d[2], -d[0]
    f = 1 / (1 + d[1])
    return np.array(((1 - z * z * f, -z, x * z * f),
                     (z, 1 - (x * x + z * z) * f, -x),
                     (x * z * f, x, 1 - x * x * f)))


def axis_change_along(direction):
    rotation, axes = axis_change_towards(direction)
    local = rotation.T @ direction
    return rotation @ turn_from_y(local / np.linalg.norm(local)), axes


class RestBones:

    def __init__(self, bones):
        count = len(bones)
        self.names = [bone[0] for bone in bones]
        self.parents = [bone[1] for bone in bones]
        self.poses = []
        frames = []
        for _, parent, location, rotation, scale in bones:
            local = np.array(transform(location, rotation, scale))
            self.poses.append(local if parent < 0 else self.poses[parent] @ local)
            frames.append(without_scale(self.poses[-1]))
        self.frames = frames

        child_counts = [0] * count
        for parent in self.parents:
            if parent >= 0:
                child_counts[parent] += 1
        away_counts, chain_counts = [0] * count, [0] * count
        away_heads, chain_heads = [np.zeros(3) for _ in range(count)], [np.zeros(3) for _ in range(count)]
        for i, parent in enumerate(self.parents):
            if parent < 0:
                continue
            head = (rigid_inverse(frames[parent]) @ frames[i])[:3, 3]
            if np.linalg.norm(head) < SHORTEST_BONE_LENGTH:
                continue
            away_counts[parent] += 1
            away_heads[parent] = away_heads[parent] + head
            if child_counts[i] == 0:
                continue
            chain_counts[parent] += 1
            chain_heads[parent] = head

        self.bones = []
        self.axis_changes = []
        self.unreal_axes = []
        self.lengths = []
        for i, parent in enumerate(self.parents):
            direction = away_heads[i]
            length = float(np.linalg.norm(direction))
            if away_counts[i] != 1 and chain_counts[i] == 1:
                direction = chain_heads[i]
                length = float(np.linalg.norm(direction))
            elif away_counts[i] > 1:
                length = BONE_DISPLAY_LENGTH
            elif away_counts[i] == 0:
                length = BONE_DISPLAY_LENGTH
                direction = np.array((1.0, 0.0, 0.0))
                if parent >= 0:
                    direction = frames[i][:3, :3].T @ self.bones[parent][:3, 1]
            rotation, axes = axis_change_along(direction)
            change = np.identity(4)
            change[:3, :3] = rotation
            self.bones.append(frames[i] @ change)
            self.axis_changes.append(change)
            self.unreal_axes.append(axes)
            self.lengths.append(length)

    def bind_local(self, i):
        parent = self.parents[i]
        return self.frames[i] if parent < 0 else rigid_inverse(self.frames[parent]) @ self.frames[i]

    def child_basis(self, i, target):
        return Matrix.Translation((0, -self.lengths[i], 0)) @ Matrix(rigid_inverse(self.bones[i]).tolist()) @ target


class BoneKeys:

    def __init__(self, rest):
        self.rest = rest
        self.index = {name: i for i, name in enumerate(rest.names)}

    def convert(self, bone, location, rotation, scale):
        i = self.index[bone]
        parent = self.rest.parents[i]
        parent_rest = np.identity(4) if parent < 0 else self.rest.bones[parent]
        parent_axes = np.identity(4) if parent < 0 else self.rest.axis_changes[parent]
        relative = rigid_inverse(parent_rest) @ self.rest.bones[i]
        to_bone = rigid_inverse(relative) @ rigid_inverse(parent_axes)
        if location is not None:
            frames, values = location
            location = frames, positions(values) @ to_bone[:3, :3].T + to_bone[:3, 3]
        if rotation is not None:
            frames, values = rotation
            before = np.array(Matrix(to_bone.tolist()).to_quaternion())
            after = np.array(Matrix(self.rest.axis_changes[i].tolist()).to_quaternion())
            rotation = frames, multiply(multiply(before, quaternions(values)), after)
        if scale is not None:
            frames, values = scale
            scale = frames, np.asarray(values).reshape(-1, 3)[:, list(self.rest.unreal_axes[i])]
        return location, rotation, scale


def light_energy(light_type, intensity):
    if light_type == "SUN":
        return intensity
    if light_type == "AREA":
        return intensity * math.pi
    return intensity * 4 * math.pi


def sky_strength(intensity):
    return intensity


def camera_lens(field_of_view):
    return 36 / (2 * np.tan(np.radians(field_of_view) / 2))
