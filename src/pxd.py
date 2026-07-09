import ctypes
import math
import os
import sys
from pathlib import Path

import bpy
from bpy.props import BoolProperty, StringProperty
from bpy_extras.io_utils import ExportHelper, ImportHelper
from mathutils import Matrix, Quaternion, Vector

from . import (
    HeatError,
    ROOT_ANIM_CORRECTION,
    SKELETON_SOURCE_PROP,
    action_export_filepath,
    clean_file_name,
    clean_bone_name,
    export_bones,
    selected_armature,
)

 # Massive thanks to AdelQue & WistfulHopes for their work on the original PXD importer/exporter.

DLLName = "HedgehogEngineAnimationTools_PXD"


def dll_names():
    if sys.platform == "win32":
        return (f"{DLLName}.windows.dll", f"{DLLName}.dll")
    if sys.platform == "darwin":
        return (f"{DLLName}.macos.dylib", f"{DLLName}.dylib")
    return (f"{DLLName}.linux.dll", f"{DLLName}.dll")


def dll_path():
    here = Path(__file__).resolve()
    candidates = []
    for name in dll_names():
        candidates.extend((here.with_name(name), here.parent / name, Path.cwd() / name))
    for candidate in candidates:
        if candidate.exists():
            return candidate
    raise HeatError(
        f"{DLLName} native library was not found. Checked:\n"
        + "\n".join(str(candidate) for candidate in candidates)
    )


def path_bytes(path):
    return os.fsencode(os.fspath(path))


def load_dll():
    try:
        dll = ctypes.CDLL(str(dll_path()))
    except OSError as ex:
        raise HeatError(str(ex)) from ex

    dll.HEAT_PXD_last_error.restype = ctypes.c_char_p

    dll.HEAT_PXD_open_skeleton.argtypes = [
        ctypes.c_char_p,
        ctypes.POINTER(ctypes.c_void_p),
    ]
    dll.HEAT_PXD_open_skeleton.restype = ctypes.c_int
    dll.HEAT_PXD_close_skeleton.argtypes = [ctypes.c_void_p]
    dll.HEAT_PXD_close_skeleton.restype = None
    dll.HEAT_PXD_skeleton_name.argtypes = [ctypes.c_void_p]
    dll.HEAT_PXD_skeleton_name.restype = ctypes.c_char_p
    dll.HEAT_PXD_skeleton_bone_count.argtypes = [ctypes.c_void_p]
    dll.HEAT_PXD_skeleton_bone_count.restype = ctypes.c_uint32
    dll.HEAT_PXD_skeleton_bone_name.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    dll.HEAT_PXD_skeleton_bone_name.restype = ctypes.c_char_p
    dll.HEAT_PXD_skeleton_bone_parent.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    dll.HEAT_PXD_skeleton_bone_parent.restype = ctypes.c_int32
    dll.HEAT_PXD_skeleton_bone_transform.argtypes = [
        ctypes.c_void_p,
        ctypes.c_uint32,
        ctypes.POINTER(ctypes.c_float),
    ]
    dll.HEAT_PXD_skeleton_bone_transform.restype = ctypes.c_int

    dll.HEAT_PXD_open_animation.argtypes = [
        ctypes.c_char_p,
        ctypes.POINTER(ctypes.c_void_p),
    ]
    dll.HEAT_PXD_open_animation.restype = ctypes.c_int
    dll.HEAT_PXD_close_animation.argtypes = [ctypes.c_void_p]
    dll.HEAT_PXD_close_animation.restype = None
    dll.HEAT_PXD_animation_fps.argtypes = [ctypes.c_void_p]
    dll.HEAT_PXD_animation_fps.restype = ctypes.c_uint32
    dll.HEAT_PXD_animation_frame_count.argtypes = [ctypes.c_void_p]
    dll.HEAT_PXD_animation_frame_count.restype = ctypes.c_uint32
    dll.HEAT_PXD_animation_track_count.argtypes = [ctypes.c_void_p]
    dll.HEAT_PXD_animation_track_count.restype = ctypes.c_uint32
    dll.HEAT_PXD_animation_sample_frame.argtypes = [
        ctypes.c_void_p,
        ctypes.c_uint32,
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_uint32,
    ]
    dll.HEAT_PXD_animation_sample_frame.restype = ctypes.c_int
    dll.HEAT_PXD_animation_root_frame_count.argtypes = [ctypes.c_void_p]
    dll.HEAT_PXD_animation_root_frame_count.restype = ctypes.c_uint32
    dll.HEAT_PXD_animation_root_sample.argtypes = [
        ctypes.c_void_p,
        ctypes.c_uint32,
        ctypes.POINTER(ctypes.c_float),
    ]
    dll.HEAT_PXD_animation_root_sample.restype = ctypes.c_int

    dll.HEAT_PXD_export_skeleton.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
    dll.HEAT_PXD_export_skeleton.restype = ctypes.c_int
    dll.HEAT_PXD_export_animation.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
    dll.HEAT_PXD_export_animation.restype = ctypes.c_int
    return dll


def last_error(dll):
    raw = dll.HEAT_PXD_last_error()
    return raw.decode("utf-8", errors="replace") if raw else "Unknown PXD error"


def write_native(func, path, text):
    dll = load_dll()
    ok = func(path_bytes(path), text.encode("utf-8"))
    if not ok:
        raise HeatError(last_error(dll))


def pxd_name(filepath):
    name = Path(filepath).name
    lowered = name.lower()
    changed = True
    while changed:
        changed = False
        for suffix in (".skl.pxd", ".anm.pxd", ".skl", ".anm", ".pxd"):
            if lowered.endswith(suffix):
                name = name[: -len(suffix)]
                lowered = name.lower()
                changed = True
                break
    return clean_file_name(name) or "Pxd"


def pxd_import_loc(values, use_yx_orientation):
    if use_yx_orientation:
        return Vector((values[2], values[0], values[1]))
    return Vector((values[0], values[1], values[2]))


def pxd_import_quat(values, use_yx_orientation):
    if use_yx_orientation:
        return Quaternion((values[3], values[2], values[0], values[1]))
    return Quaternion((values[3], values[0], values[1], values[2]))


def pxd_bone_name(value, fallback):
    name = str(value or "").replace("\t", " ").replace("\r", " ").replace("\n", " ")
    return name or fallback


def pxd_decode(value):
    return (value or b"").decode("utf-8", errors="replace")


def pxd_values(values):
    return [float(values[i]) for i in range(10)]


def pxd_sample_matrix(values, pose_bone, use_yx_orientation):
    sample = pxd_values(values)
    if use_yx_orientation:
        rotation = Quaternion((sample[6], sample[5], sample[3], sample[4]))
        if not pose_bone.parent:
            rotation @= ROOT_ANIM_CORRECTION
        location = Vector((sample[2], sample[0], sample[1]))
    else:
        rotation = Quaternion((sample[6], sample[3], sample[4], sample[5]))
        location = Vector((sample[0], sample[1], sample[2]))
    return Matrix.LocRotScale(location, rotation, Vector((1.0, 1.0, 1.0)))


def pxd_sample_scale(values, use_yx_orientation):
    sample = pxd_values(values)
    if tuple(sample[7:10]) == (0.0, 0.0, 0.0):
        return Vector((1.0, 1.0, 1.0))
    if use_yx_orientation:
        return Vector((sample[9], sample[7], sample[8]))
    return Vector((sample[7], sample[8], sample[9]))


def pxd_get_matrix_map_global(obj, matrix_map_local, scale_map):
    matrix_map_global = {}
    for pbone in obj.pose.bones:
        matrix = Matrix()
        scale = scale_map[pbone.name].copy()
        for parent_bone in reversed(pbone.parent_recursive):
            if parent_bone.name in matrix_map_local:
                matrix @= matrix_map_local[parent_bone.name]
                scale *= scale_map[parent_bone.name]
        matrix @= matrix_map_local[pbone.name]
        loc, rot, _ = matrix.decompose()
        matrix_map_global[pbone.name] = Matrix.LocRotScale(loc, rot, scale)
    return matrix_map_global


def pxd_set_pose_matrices_global(obj, matrix_map_global, frame):
    def rec(pbone, parent_matrix):
        if pbone.name in matrix_map_global:
            matrix = matrix_map_global[pbone.name].copy()
            if pbone.parent:
                pbone.matrix_basis = pbone.bone.convert_local_to_pose(
                    matrix,
                    pbone.bone.matrix_local,
                    parent_matrix=parent_matrix,
                    parent_matrix_local=pbone.parent.bone.matrix_local,
                    invert=True,
                )
            else:
                pbone.matrix_basis = pbone.bone.convert_local_to_pose(
                    matrix,
                    pbone.bone.matrix_local,
                    invert=True,
                )
        else:
            if pbone.parent:
                matrix = pbone.bone.convert_local_to_pose(
                    pbone.matrix_basis,
                    pbone.bone.matrix_local,
                    parent_matrix=parent_matrix,
                    parent_matrix_local=pbone.parent.bone.matrix_local,
                )
            else:
                matrix = pbone.bone.convert_local_to_pose(
                    pbone.matrix_basis,
                    pbone.bone.matrix_local,
                )

        pbone.keyframe_insert("location", frame=frame)
        pbone.keyframe_insert("rotation_quaternion", frame=frame)
        pbone.keyframe_insert("scale", frame=frame)

        for child in pbone.children:
            rec(child, matrix)

    for pbone in obj.pose.bones:
        if not pbone.parent:
            rec(pbone, None)


def pxd_neutral_pose_bone(pose_bone):
    pose_bone.location = (0.0, 0.0, 0.0)
    pose_bone.rotation_mode = "QUATERNION"
    pose_bone.rotation_quaternion = (1.0, 0.0, 0.0, 0.0)
    pose_bone.scale = (1.0, 1.0, 1.0)


def pxd_key_neutral_pose_bone(pose_bone, frame):
    pxd_neutral_pose_bone(pose_bone)
    pose_bone.keyframe_insert("location", frame=frame)
    pose_bone.keyframe_insert("rotation_quaternion", frame=frame)
    pose_bone.keyframe_insert("scale", frame=frame)


def create_pxd_armature_from_skeleton_handle(dll, handle, object_name, use_yx_orientation):
    arm_data = bpy.data.armatures.new(object_name)
    arm_obj = bpy.data.objects.new(object_name, arm_data)
    arm_obj.show_in_front = True

    bpy.context.collection.objects.link(arm_obj)
    bpy.ops.object.select_all(action="DESELECT")
    bpy.context.view_layer.objects.active = arm_obj
    arm_obj.select_set(True)
    arm_obj.rotation_euler = (math.pi * 0.5, 0.0, 0.0)

    bone_count = int(dll.HEAT_PXD_skeleton_bone_count(handle))
    names = []
    parents = []
    transforms = []
    values = (ctypes.c_float * 10)()
    for i in range(bone_count):
        names.append(pxd_bone_name(pxd_decode(dll.HEAT_PXD_skeleton_bone_name(handle, i)), f"Bone_{i:03d}"))
        parents.append(int(dll.HEAT_PXD_skeleton_bone_parent(handle, i)))
        if not dll.HEAT_PXD_skeleton_bone_transform(handle, i, values):
            raise HeatError("Failed to read PXD skeleton bone transform")
        transforms.append(pxd_values(values))

    bpy.ops.object.mode_set(mode="EDIT")
    edit_bones = []
    for i, name in enumerate(names):
        edit_bone = arm_data.edit_bones.new(name)
        edit_bone.use_connect = False
        edit_bone.use_inherit_rotation = True
        edit_bone.inherit_scale = "ALIGNED"
        edit_bone.use_local_location = True
        edit_bone.head = (0.0, 0.0, 0.0)
        if use_yx_orientation:
            edit_bone.tail = (0.1, 0.0, 0.0)
            edit_bone.roll = -math.pi * 0.5
        else:
            edit_bone.tail = (0.0, 0.1, 0.0)
        edit_bones.append(edit_bone)

    for i, parent in enumerate(parents):
        if parent >= 0:
            edit_bones[i].parent = edit_bones[parent]
            edit_bones[i].use_connect = False

    bpy.ops.object.mode_set(mode="POSE")
    for i, pose_bone in enumerate(arm_obj.pose.bones):
        transform = transforms[i]
        pose_bone.rotation_mode = "QUATERNION"
        pose_bone.rotation_quaternion = pxd_import_quat(
            transform[3:7],
            use_yx_orientation,
        )
        pose_bone.location = pxd_import_loc(
            transform[0:3],
            use_yx_orientation,
        )

    bpy.ops.pose.armature_apply(selected=False)
    bpy.ops.object.mode_set(mode="EDIT")
    for edit_bone in arm_data.edit_bones:
        child_bone = None
        child_score = -1
        for candidate in edit_bone.children:
            score = len(candidate.children_recursive)
            if child_bone is None or score > child_score:
                child_bone = candidate
                child_score = score

        if child_bone:
            length = (edit_bone.head - child_bone.head).length
        elif edit_bone.parent:
            length = edit_bone.parent.length
        else:
            length = 0.025
        edit_bone.length = max(0.025, min(0.6, length))
        edit_bone.inherit_scale = "ALIGNED"

    bpy.ops.object.mode_set(mode="OBJECT")
    arm_data.name = object_name
    arm_obj.name = object_name
    return arm_obj


def pxd_skeleton_values(matrix, use_yx_orientation):
    loc, rot, _ = matrix.decompose()
    if use_yx_orientation:
        return (
            loc.y,
            loc.z,
            loc.x,
            rot.y,
            rot.z,
            rot.x,
            rot.w,
            1.0,
            1.0,
            1.0,
        )
    return (
        loc.x,
        loc.y,
        loc.z,
        rot.x,
        rot.y,
        rot.z,
        rot.w,
        1.0,
        1.0,
        1.0,
    )


def pxd_sanitize(value):
    return str(value).replace("\t", " ").replace("\r", " ").replace("\n", " ")


def pxd_float(value):
    return f"{float(value):.17g}"


def serialize_pxd_skeleton(arm_obj, use_yx_orientation=True):
    entries = export_bones(arm_obj, preserve_order=True)
    lines = [f"skeleton\t{pxd_sanitize(arm_obj.name or arm_obj.data.name)}\t{len(entries)}"]
    correction = Quaternion((0.5, 0.5, 0.5, 0.5))

    for entry in entries:
        matrix = entry["matrix"].copy()
        if use_yx_orientation and entry["parent"] < 0:
            loc, rot, scale = matrix.decompose()
            matrix = Matrix.LocRotScale(loc, rot @ correction, scale)
        values = pxd_skeleton_values(matrix, use_yx_orientation)
        lines.append(
            "bone\t{}\t{}\t{}".format(
                entry["parent"],
                pxd_bone_name(entry["name"], f"Bone_{len(lines) - 1:03d}"),
                "\t".join(pxd_float(v) for v in values),
            )
        )
    return "\n".join(lines)


def pxd_animation_values(loc, rot, scale, use_yx_orientation, is_root):
    if use_yx_orientation:
        if is_root:
            rot = rot @ Quaternion((0.5, 0.5, 0.5, 0.5))
        return (
            (loc.y, loc.z, loc.x),
            (rot.y, rot.z, rot.x, rot.w),
            (scale.y, scale.z, scale.x),
        )
    return (
        (loc.x, loc.y, loc.z),
        (rot.x, rot.y, rot.z, rot.w),
        (scale.x, scale.y, scale.z),
    )


def serialize_pxd_animation(arm_obj, use_yx_orientation=True, root_motion=True, additive=False):
    action = arm_obj.animation_data.action if arm_obj.animation_data else None
    if not action:
        raise HeatError("The selected armature has no active action")

    scene = bpy.context.scene
    frame_rate = scene.render.fps / scene.render.fps_base
    start = int(scene.frame_start)
    end = int(scene.frame_end)
    frame_count = max(1, end - start + 1)
    duration = (frame_count - 1) / frame_rate if frame_count > 1 and frame_rate else 0.0
    pose_bones = list(arm_obj.pose.bones)

    lines = [f"animation\t{pxd_float(frame_rate)}\t{pxd_float(duration)}\t{frame_count}"]
    for i, pbone in enumerate(pose_bones):
        lines.append(f"track\t{i}\t{pxd_bone_name(pbone.name, f'Bone_{i:03d}')}")
    if root_motion:
        lines.append(f"rootmotion\t{pxd_float(duration)}\t0\t1\t0\t0\t0\t0\t1\t0")

    current_frame = scene.frame_current
    current_subframe = scene.frame_subframe
    inherit_scale_modes = {bone.name: bone.inherit_scale for bone in arm_obj.data.bones}
    try:
        for bone in arm_obj.data.bones:
            bone.inherit_scale = 'ALIGNED'
        for out_frame in range(frame_count):
            scene.frame_set(start + out_frame)
            bpy.context.view_layer.update()

            matrix_map = {}
            scale_map = {}
            for pbone in pose_bones:
                loc, rot, scale = pbone.matrix.decompose()
                if pbone.parent:
                    parent_scale = pbone.parent.matrix.to_scale()
                    scale = Vector((scale.x / parent_scale.x, scale.y / parent_scale.y, scale.z / parent_scale.z))
                matrix_map[pbone.name] = Matrix.LocRotScale(loc, rot, Vector((1.0, 1.0, 1.0)))
                scale_map[pbone.name] = scale

            for i, pbone in enumerate(pose_bones):
                parent_matrix = matrix_map[pbone.parent.name] if pbone.parent else Matrix()
                local = parent_matrix.inverted() @ matrix_map[pbone.name]
                loc, rot, _ = local.decompose()
                scale = scale_map[pbone.name]
                t, r, s = pxd_animation_values(loc, rot, scale, use_yx_orientation, not pbone.parent)
                bone_length = pbone.length if pbone.parent else 0.0
                translation_w = bone_length * (scale.y if use_yx_orientation else scale.x)
                lines.append(
                    "frame\t{}\t{}\t{}\t{}\t{}\t{}\t1".format(
                        i,
                        out_frame,
                        "\t".join(pxd_float(v) for v in t),
                        "\t".join(pxd_float(v) for v in r),
                        "\t".join(pxd_float(v) for v in s),
                        pxd_float(translation_w),
                    )
                )

            if root_motion:
                loc = arm_obj.location.copy()
                rot = Quaternion((2 ** -0.5, -(2 ** -0.5), 0.0, 0.0)) @ arm_obj.rotation_quaternion.copy()
                scale = arm_obj.scale.copy()
                lines.append(
                    "rootframe\t{}\t{}\t{}\t{}\t0\t1".format(
                        out_frame,
                        "\t".join(pxd_float(v) for v in (loc.x, loc.z, -loc.y)),
                        "\t".join(pxd_float(v) for v in (rot.x, rot.y, rot.z, rot.w)),
                        "\t".join(pxd_float(v) for v in (scale.x, scale.y, scale.z)),
                    )
                )
    finally:
        for bone in arm_obj.data.bones:
            bone.inherit_scale = inherit_scale_modes.get(bone.name, bone.inherit_scale)
        scene.frame_set(current_frame, subframe=current_subframe)

    lines.append(f"pxdflags\t{int(additive)}\t{int(root_motion)}")
    return "\n".join(lines)


def apply_pxd_root_sample(arm_obj, values, frame):
    arm_obj.location = (values[0], -values[2], values[1])
    arm_obj.rotation_mode = "QUATERNION"
    raw_rotation = Quaternion((values[6], values[3], values[4], values[5]))
    arm_obj.rotation_quaternion = Quaternion((2 ** -0.5, 2 ** -0.5, 0.0, 0.0)) @ raw_rotation
    if tuple(values[7:10]) == (0.0, 0.0, 0.0):
        arm_obj.scale = (1.0, 1.0, 1.0)
    else:
        arm_obj.scale = (values[7], values[8], values[9])
    arm_obj.keyframe_insert("location", frame=frame)
    arm_obj.keyframe_insert("rotation_quaternion", frame=frame)
    arm_obj.keyframe_insert("scale", frame=frame)


def apply_pxd_animation_handle_to_armature(dll, handle, arm_obj, filepath, use_yx_orientation=True, root_motion=True):
    if not arm_obj:
        raise HeatError("Select an armature before importing an animation")

    track_count = int(dll.HEAT_PXD_animation_track_count(handle))
    frame_total = max(1, int(dll.HEAT_PXD_animation_frame_count(handle)))
    fps = max(1, int(dll.HEAT_PXD_animation_fps(handle)))
    action_name = clean_bone_name(pxd_name(filepath)) or "PxdAnimation"
    action = bpy.data.actions.new(action_name)
    arm_obj.animation_data_create()
    arm_obj.animation_data.action = action
    if hasattr(action, "use_frame_range"):
        action.use_frame_range = True
        action.frame_start = 0
        action.frame_end = frame_total - 1

    scene = bpy.context.scene
    scene.frame_start = 0
    scene.frame_end = frame_total - 1
    scene.render.fps = fps

    current_frame = scene.frame_current

    bpy.ops.object.select_all(action="DESELECT")
    bpy.context.view_layer.objects.active = arm_obj
    arm_obj.select_set(True)
    bpy.ops.object.mode_set(mode="POSE")

    pose_bones = arm_obj.pose.bones
    bone_names = [bone.name for bone in arm_obj.data.bones]
    animated_names = set()

    for pb in pose_bones:
        pxd_neutral_pose_bone(pb)

    track_targets = []
    for track in range(track_count):
        if track >= len(bone_names):
            continue
        name = bone_names[track]
        if name not in pose_bones:
            continue
        track_targets.append((track, name))
        animated_names.add(name)

    sample_values = (ctypes.c_float * (track_count * 10))()
    root_values = (ctypes.c_float * 10)()
    root_frame_count = int(dll.HEAT_PXD_animation_root_frame_count(handle)) if root_motion else 0
    for frame in range(frame_total):
        if not dll.HEAT_PXD_animation_sample_frame(handle, frame, sample_values, len(sample_values)):
            raise HeatError("Failed to read PXD animation frame")

        matrix_map_local = {}
        scale_map = {}

        for pb in pose_bones:
            matrix_map_local[pb.name] = Matrix()
            scale_map[pb.name] = Vector((1.0, 1.0, 1.0))

        for track, name in track_targets:
            base = track * 10
            pb = pose_bones[name]
            sample = sample_values[base:base + 10]
            matrix_map_local[name] = pxd_sample_matrix(sample, pb, use_yx_orientation)
            scale_map[name] = pxd_sample_scale(sample, use_yx_orientation)

        matrix_map_global = pxd_get_matrix_map_global(arm_obj, matrix_map_local, scale_map)
        pxd_set_pose_matrices_global(arm_obj, matrix_map_global, frame)

        if frame < root_frame_count and dll.HEAT_PXD_animation_root_sample(handle, frame, root_values):
            apply_pxd_root_sample(arm_obj, root_values, frame)

    last_frame = frame_total - 1
    for pb in pose_bones:
        if pb.name in animated_names:
            continue
        pxd_key_neutral_pose_bone(pb, 0)
        if last_frame:
            pxd_key_neutral_pose_bone(pb, last_frame)

    bpy.ops.object.mode_set(mode="OBJECT")
    scene.frame_set(0)
    bpy.context.view_layer.update()
    scene.frame_set(current_frame if current_frame <= last_frame else 0)
    return action


def import_pxd_animation_file(filepath, arm_obj, use_yx_orientation=True, root_motion=True, context=None):
    if context:
        context.scene.heat_last_animation_path = filepath
    dll = load_dll()
    handle = ctypes.c_void_p()
    if not dll.HEAT_PXD_open_animation(path_bytes(filepath), ctypes.byref(handle)):
        raise HeatError(last_error(dll))
    action = apply_pxd_animation_handle_to_armature(
        dll,
        handle,
        arm_obj,
        filepath,
        use_yx_orientation,
        root_motion,
    )
    dll.HEAT_PXD_close_animation(handle)
    return action


def export_pxd_animation_file(filepath, arm_obj, use_yx_orientation=True, root_motion=True, additive=False):
    dll = load_dll()
    animation = serialize_pxd_animation(
        arm_obj,
        use_yx_orientation,
        root_motion,
        additive,
    )
    write_native(dll.HEAT_PXD_export_animation, filepath, animation)


class HEAT_OT_import_pxd_skeleton(bpy.types.Operator, ImportHelper):
    bl_idname = "heat.import_pxd_skeleton"
    bl_label = "PXD Skeleton (.skl.pxd)"
    bl_options = {"REGISTER", "UNDO"}
    filename_ext = ".pxd"
    filter_glob: StringProperty(default="*.skl.pxd;*.pxd", options={"HIDDEN"})
    use_yx_orientation: BoolProperty(name="Convert to YX Bone Orientation", default=True)

    def draw(self, context):
        self.layout.prop(self, "use_yx_orientation")

    def execute(self, context):
        try:
            dll = load_dll()
            handle = ctypes.c_void_p()
            if not dll.HEAT_PXD_open_skeleton(path_bytes(self.filepath), ctypes.byref(handle)):
                raise HeatError(last_error(dll))
            fallback = pxd_name(self.filepath)
            arm_obj = create_pxd_armature_from_skeleton_handle(
                dll,
                handle,
                fallback,
                self.use_yx_orientation,
            )
            dll.HEAT_PXD_close_skeleton(handle)
            arm_obj[SKELETON_SOURCE_PROP] = self.filepath
        except Exception as ex:
            self.report({"ERROR"}, str(ex))
            return {"CANCELLED"}
        return {"FINISHED"}


class HEAT_OT_import_pxd_animation(bpy.types.Operator, ImportHelper):
    bl_idname = "heat.import_pxd_animation"
    bl_label = "PXD Animation (.anm.pxd)"
    bl_options = {"REGISTER", "UNDO"}
    filename_ext = ".pxd"
    filter_glob: StringProperty(default="*.anm.pxd;*.pxd", options={"HIDDEN"})
    use_yx_orientation: BoolProperty(name="Use YX Bone Orientation", default=True)
    root_motion: BoolProperty(name="Import Root Motion", default=True)

    def draw(self, context):
        self.layout.prop(self, "use_yx_orientation")
        self.layout.prop(self, "root_motion")

    def execute(self, context):
        try:
            arm_obj = selected_armature()
            if not arm_obj:
                raise HeatError("Select an armature before importing an animation")
            import_pxd_animation_file(
                self.filepath,
                arm_obj,
                self.use_yx_orientation,
                self.root_motion,
                context,
            )
        except Exception as ex:
            self.report({"ERROR"}, str(ex))
            return {"CANCELLED"}
        return {"FINISHED"}


class HEAT_OT_export_pxd_skeleton(bpy.types.Operator, ExportHelper):
    bl_idname = "heat.export_pxd_skeleton"
    bl_label = "PXD Skeleton (.skl.pxd)"
    filename_ext = ".skl.pxd"
    check_extension = False
    filter_glob: StringProperty(default="*.skl.pxd;*.pxd", options={"HIDDEN"})
    use_yx_orientation: BoolProperty(name="Convert from YX Bone Orientation", default=True)

    def draw(self, context):
        self.layout.prop(self, "use_yx_orientation")

    def execute(self, context):
        try:
            arm_obj = selected_armature()
            if not arm_obj:
                raise HeatError("Select an armature before exporting a skeleton")
            dll = load_dll()
            write_native(
                dll.HEAT_PXD_export_skeleton,
                self.filepath,
                serialize_pxd_skeleton(arm_obj, self.use_yx_orientation),
            )
        except Exception as ex:
            self.report({"ERROR"}, str(ex))
            return {"CANCELLED"}
        return {"FINISHED"}


class HEAT_OT_export_pxd_animation(bpy.types.Operator, ExportHelper):
    bl_idname = "heat.export_pxd_animation"
    bl_label = "PXD Animation (.anm.pxd)"
    filename_ext = ".anm.pxd"
    check_extension = False
    filter_glob: StringProperty(default="*.anm.pxd;*.pxd", options={"HIDDEN"})
    use_yx_orientation: BoolProperty(name="Use YX Bone Orientation", default=True)
    root_motion: BoolProperty(name="Export Root Motion", default=True)
    additive: BoolProperty(name="Flag As Additive", default=False)

    def draw(self, context):
        self.layout.prop(self, "use_yx_orientation")
        self.layout.prop(self, "root_motion")
        self.layout.prop(self, "additive")

    def invoke(self, context, event):
        if not self.filepath:
            self.filepath = action_export_filepath(context, self.filename_ext)
        context.window_manager.fileselect_add(self)
        return {"RUNNING_MODAL"}

    def execute(self, context):
        try:
            arm_obj = selected_armature()
            if not arm_obj:
                raise HeatError("Select an armature before exporting an animation")
            export_pxd_animation_file(
                self.filepath,
                arm_obj,
                self.use_yx_orientation,
                self.root_motion,
                self.additive,
            )
        except Exception as ex:
            self.report({"ERROR"}, str(ex))
            return {"CANCELLED"}
        return {"FINISHED"}


CLASSES = (
    HEAT_OT_import_pxd_skeleton,
    HEAT_OT_import_pxd_animation,
    HEAT_OT_export_pxd_skeleton,
    HEAT_OT_export_pxd_animation,
)


def draw_import_menu(layout):
    layout.operator(HEAT_OT_import_pxd_skeleton.bl_idname, text="PXD Skeleton (.skl.pxd)")
    layout.operator(HEAT_OT_import_pxd_animation.bl_idname, text="PXD Animation (.anm.pxd)")


def draw_export_menu(layout):
    layout.operator(HEAT_OT_export_pxd_skeleton.bl_idname, text="PXD Skeleton (.skl.pxd)")
    layout.operator(HEAT_OT_export_pxd_animation.bl_idname, text="PXD Animation (.anm.pxd)")


def register():
    for cls in CLASSES:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(CLASSES):
        bpy.utils.unregister_class(cls)
