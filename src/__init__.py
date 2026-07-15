bl_info = {
    "name": "Hedgehog Engine Animation Tools",
    "author": "EM",
    "version": (0, 1, 0),
    "blender": (4, 0, 0),
    "location": "File > Import/Export > Hedgehog Engine Animation",
    "description": "Imports and exports Hedgehog Engine Havok skeletons and animations",
    "category": "Import-Export",
}

import ctypes
import math
import os
import re
import sys
from pathlib import Path

import bpy
from bpy.props import BoolProperty, EnumProperty, StringProperty
from bpy_extras.io_utils import ExportHelper, ImportHelper
from mathutils import Matrix, Quaternion, Vector


DLLName = "HedgehogEngineAnimationTools"
ORIENTATION_EULER = (math.radians(90.0), 0.0, 0.0)
EXPORT_ANIMATION_FPS = 60
ROOT_ANIM_CORRECTION = Quaternion((0.5, -0.5, -0.5, -0.5))
ROOT_EXPORT_CORRECTION = Quaternion((0.5, 0.5, 0.5, 0.5))
ACTION_FPS_PROP = "heat_action_fps"
SKELETON_SOURCE_PROP = "heat_skeleton_source_path"


class HeatError(RuntimeError):
    pass


def dll_names():
    if sys.platform == "win32":
        platform_name = f"{DLLName}.windows.dll"
    elif sys.platform == "linux":
        platform_name = f"{DLLName}.linux.dll"
    elif sys.platform == "darwin":
        platform_name = f"{DLLName}.macos.dylib"
    else:
        platform_name = f"{DLLName}.dll"
    return (platform_name, f"{DLLName}.dll")


def dll_path():
    here = Path(__file__).resolve()
    candidates = []
    for dll_name in dll_names():
        candidates.extend(
            [
                here.with_name(dll_name),
                here.parent / dll_name,
                here.parent.parent / dll_name,
                Path.cwd() / dll_name,
            ]
        )

    for candidate in candidates:
        if candidate.exists():
            return candidate

    checked = "\n".join(str(candidate) for candidate in candidates)
    raise HeatError(f"{DLLName} native library was not found. Checked:\n{checked}")


def path_bytes(path):
    return os.fsencode(os.fspath(path))


def load_dll():
    path = dll_path()
    try:
        dll = ctypes.CDLL(str(path))
    except OSError as ex:
        raise HeatError(f"Could not load {path}: {ex}") from ex
    dll.HEAT_last_error.restype = ctypes.c_char_p

    dll.HEAT_open_skeleton.argtypes = [
        ctypes.c_char_p,
        ctypes.POINTER(ctypes.c_void_p),
    ]
    dll.HEAT_open_skeleton.restype = ctypes.c_int
    dll.HEAT_close_skeleton.argtypes = [ctypes.c_void_p]
    dll.HEAT_close_skeleton.restype = None
    dll.HEAT_skeleton_name.argtypes = [ctypes.c_void_p]
    dll.HEAT_skeleton_name.restype = ctypes.c_char_p
    dll.HEAT_skeleton_bone_count.argtypes = [ctypes.c_void_p]
    dll.HEAT_skeleton_bone_count.restype = ctypes.c_uint32
    dll.HEAT_skeleton_bone_name.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    dll.HEAT_skeleton_bone_name.restype = ctypes.c_char_p
    dll.HEAT_skeleton_bone_parent.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    dll.HEAT_skeleton_bone_parent.restype = ctypes.c_int32
    dll.HEAT_skeleton_bone_transform.argtypes = [
        ctypes.c_void_p,
        ctypes.c_uint32,
        ctypes.POINTER(ctypes.c_float),
    ]
    dll.HEAT_skeleton_bone_transform.restype = ctypes.c_int

    dll.HEAT_open_animation.argtypes = [
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.POINTER(ctypes.c_void_p),
    ]
    dll.HEAT_open_animation.restype = ctypes.c_int
    dll.HEAT_close_animation.argtypes = [ctypes.c_void_p]
    dll.HEAT_close_animation.restype = None
    dll.HEAT_animation_fps.argtypes = [ctypes.c_void_p]
    dll.HEAT_animation_fps.restype = ctypes.c_uint32
    dll.HEAT_animation_frame_count.argtypes = [ctypes.c_void_p]
    dll.HEAT_animation_frame_count.restype = ctypes.c_uint32
    dll.HEAT_animation_track_count.argtypes = [ctypes.c_void_p]
    dll.HEAT_animation_track_count.restype = ctypes.c_uint32
    dll.HEAT_animation_track_name.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    dll.HEAT_animation_track_name.restype = ctypes.c_char_p
    dll.HEAT_animation_sample_frame.argtypes = [
        ctypes.c_void_p,
        ctypes.c_uint32,
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_uint32,
    ]
    dll.HEAT_animation_sample_frame.restype = ctypes.c_int

    dll.HEAT_export_skeleton.argtypes = [
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_int,
    ]
    dll.HEAT_export_skeleton.restype = ctypes.c_int

    dll.HEAT_export_animation.argtypes = [
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_int,
    ]
    dll.HEAT_export_animation.restype = ctypes.c_int

    return dll


def last_error(dll):
    raw = dll.HEAT_last_error()
    if not raw:
        return "Unknown Hedgehog Engine Animation Tools error"
    return raw.decode("utf-8", errors="replace")


def native_quat(values):
    return Quaternion((values[3], values[0], values[1], values[2]))


def hk_to_blender_loc(values):
    return Vector((values[2], values[0], values[1]))


def hk_to_blender_root_motion_loc(values):
    return Vector((values[0], values[1], values[2]))


def hk_to_blender_quat(values, root_animation=False):
    quat = Quaternion((values[3], values[2], values[0], values[1]))
    if root_animation:
        quat @= ROOT_ANIM_CORRECTION
    return quat


def hk_to_blender_scale(values):
    if tuple(values) == (0.0, 0.0, 0.0):
        return Vector((1.0, 1.0, 1.0))
    return Vector((values[2], values[0], values[1]))


def native_matrix(transform):
    return Matrix.LocRotScale(
        hk_to_blender_loc(transform["translation"]),
        hk_to_blender_quat(transform["rotation"]),
        Vector((1.0, 1.0, 1.0)),
    )


def native_matrix_values(values):
    return Matrix.LocRotScale(
        hk_to_blender_loc(values[0:3]),
        hk_to_blender_quat(values[3:7]),
        Vector((1.0, 1.0, 1.0)),
    )


def bone_length(index, globals_, children):
    head = globals_[index].translation
    for child in children[index]:
        delta = globals_[child].translation - head
        if delta.length > 0.001:
            return delta.length
    return 0.08


def clamped_bone_length(index, globals_, children):
    length = bone_length(index, globals_, children)
    return max(0.025, min(0.6, length))


def clean_file_name(name):
    name = os.path.basename(str(name or "")).strip()
    changed = True
    while changed:
        before = name
        name = re.sub(r"\.\d{3}$", "", name).strip()
        name = re.sub(r"\s*\(\d+\)$", "", name).strip()
        lowered = name.lower()
        for suffix in (".skl.hkx", ".anm.hkx"):
            if lowered.endswith(suffix):
                name = name[:-len(suffix)].strip()
                break
        changed = name != before
    return name


def strip_at_lt(name):
    name = str(name or "")
    match = re.search(r"@lt$", name, flags=re.IGNORECASE)
    if match:
        return name[:match.start()]
    return name


def clean_bone_name(name):
    return strip_at_lt(clean_file_name(name))


def bone_key(name):
    return clean_bone_name(name).lower()


def match_bone_name(name):
    return clean_bone_name(name).lower()

def add_lookup(lookup, key, value):
    key = str(key or "").strip().lower()
    if key and key not in lookup:
        lookup[key] = value

def build_bone_lookup(arm_obj):
    lookup = {}
    for bone in arm_obj.data.bones:
        raw = clean_bone_name(bone.name)
        matched = match_bone_name(raw)
        add_lookup(lookup, raw, bone.name)
        add_lookup(lookup, matched, bone.name)
        if matched:
            add_lookup(lookup, matched + "@lt", bone.name)
    return lookup

def find_bone_name(track_name, bone_index, bone_names, pose_lookup):
    if 0 <= bone_index < len(bone_names):
        return bone_names[bone_index]

    name = clean_file_name(track_name)
    return (
        pose_lookup.get(str(name).lower())
        or pose_lookup.get(match_bone_name(name))
        or name
    )


def skeleton_object_name(data):
    bones = data.get("bones") or []
    if bones:
        root = next((bone for bone in bones if bone.get("parent", -1) < 0), bones[0])
        root_name = clean_bone_name(root.get("name"))
        if root_name:
            return root_name

    name = clean_bone_name(data.get("name"))
    if name:
        return name

    return "HavokSkeleton"


def set_armature_transform(arm_obj):
    arm_obj.rotation_mode = "XYZ"
    arm_obj.rotation_euler = ORIENTATION_EULER
    arm_obj.rotation_quaternion = (1.0, 0.0, 0.0, 0.0)
    arm_obj.delta_rotation_euler = (0.0, 0.0, 0.0)
    arm_obj.delta_rotation_quaternion = (1.0, 0.0, 0.0, 0.0)
    arm_obj.scale = (1.0, 1.0, 1.0)
    arm_obj.delta_scale = (1.0, 1.0, 1.0)
    arm_obj.update_tag(refresh={"OBJECT", "DATA"})


def set_rotation_ui(arm_obj):
    bpy.context.view_layer.objects.active = arm_obj
    arm_obj.select_set(True)
    arm_obj.rotation_mode = "XYZ"
    arm_obj.rotation_euler = ORIENTATION_EULER
    arm_obj.rotation_quaternion = (1.0, 0.0, 0.0, 0.0)
    arm_obj.scale = (1.0, 1.0, 1.0)
    for pb in arm_obj.pose.bones:
        old_matrix = pb.matrix_basis.copy()
        pb.rotation_mode = "XYZ"
        pb.matrix_basis = old_matrix
        pb.scale = (1.0, 1.0, 1.0)
    bpy.context.view_layer.update()


def defer_set_rotation_ui(arm_obj):
    def run_deferred():
        if arm_obj and arm_obj.name in bpy.data.objects:
            set_rotation_ui(arm_obj)
        return None
    try:
        bpy.app.timers.register(run_deferred, first_interval=0.05)
    except Exception:
        pass


def create_armature_from_skeleton_handle(dll, handle, object_name=None):
    arm_data = bpy.data.armatures.new(object_name or "HavokSkeleton")
    arm_obj = bpy.data.objects.new(arm_data.name, arm_data)
    bpy.context.collection.objects.link(arm_obj)
    bpy.ops.object.select_all(action="DESELECT")
    bpy.context.view_layer.objects.active = arm_obj
    arm_obj.select_set(True)
    set_armature_transform(arm_obj)

    bone_count = int(dll.HEAT_skeleton_bone_count(handle))
    names = []
    parents = []
    local_mats = []
    global_mats = [Matrix.Identity(4) for _ in range(bone_count)]
    children = [[] for _ in range(bone_count)]
    values = (ctypes.c_float * 10)()

    for i in range(bone_count):
        bone_name = (dll.HEAT_skeleton_bone_name(handle, i) or b"").decode("utf-8", errors="replace")
        names.append(clean_bone_name(bone_name) or f"Bone_{i:03d}")
        parent = int(dll.HEAT_skeleton_bone_parent(handle, i))
        parents.append(parent)
        if not dll.HEAT_skeleton_bone_transform(handle, i, values):
            raise HeatError("Failed to read skeleton bone transform")
        local_mats.append(native_matrix_values(values))

    for i, parent in enumerate(parents):
        if parent >= 0:
            children[parent].append(i)
            global_mats[i] = global_mats[parent] @ local_mats[i]
        else:
            global_mats[i] = local_mats[i]

    bpy.ops.object.mode_set(mode="EDIT")

    edit_bones = []
    for i, name in enumerate(names):
        eb = arm_data.edit_bones.new(name)
        eb.use_connect = False
        eb.use_inherit_rotation = True
        eb.use_local_location = True
        eb.inherit_scale = "ALIGNED"
        eb.head = (0.0, 0.0, 0.0)
        eb.tail = (0.1, 0.0, 0.0)
        eb.roll = -math.pi * 0.5
        edit_bones.append(eb)

    for i, parent in enumerate(parents):
        if parent >= 0:
            edit_bones[i].parent = edit_bones[parent]
            edit_bones[i].use_connect = False

    bpy.ops.object.mode_set(mode="POSE")

    for i, pose_bone in enumerate(arm_obj.pose.bones):
        pose_bone.rotation_mode = "QUATERNION"
        pose_bone.location = local_mats[i].translation
        pose_bone.rotation_quaternion = local_mats[i].to_quaternion()
        pose_bone.scale = (1.0, 1.0, 1.0)

    bpy.ops.pose.armature_apply(selected=False)

    bpy.ops.object.mode_set(mode="EDIT")

    for i, edit_bone in enumerate(arm_data.edit_bones):
        edit_bone.length = clamped_bone_length(i, global_mats, children)
        edit_bone.inherit_scale = "ALIGNED"

    bpy.ops.object.mode_set(mode="OBJECT")
    set_armature_transform(arm_obj)
    set_rotation_ui(arm_obj)
    defer_set_rotation_ui(arm_obj)
    arm_data.name = clean_bone_name(arm_data.name) or "HavokSkeleton"
    arm_obj.name = arm_data.name
    arm_data.show_axes = False

    for pose_bone in arm_obj.pose.bones:
        pose_bone.rotation_mode = "XYZ"
        pose_bone.bone.inherit_scale = "ALIGNED"
        pose_bone.scale = (1.0, 1.0, 1.0)

    return arm_obj


def selected_armature():
    obj = bpy.context.object
    if obj and obj.type == "ARMATURE":
        return obj
    for obj in bpy.context.selected_objects:
        if obj.type == "ARMATURE":
            return obj
    return None


def rest_matrices(arm_obj):
    rest = {}
    for bone in arm_obj.data.bones:
        if bone.parent:
            rest[bone.name] = bone.parent.matrix_local.inverted() @ bone.matrix_local
        else:
            rest[bone.name] = bone.matrix_local.copy()
    return rest


def sample_matrix(sample):
    return Matrix.LocRotScale(
        hk_to_blender_loc(sample[0:3]),
        hk_to_blender_quat(sample[3:7]),
        Vector((1.0, 1.0, 1.0)),
    )


def sample_scale(sample):
    return hk_to_blender_scale(sample[7:10])


def reference_cancel_loc(values):
    return Vector((-values[2], values[0], values[1]))


def sample_matrix_for_bone(sample, pose_bone):
    if pose_bone.parent is None:
        return Matrix.LocRotScale(
            hk_to_blender_root_motion_loc(sample[0:3]),
            hk_to_blender_quat(sample[3:7], root_animation=True),
            Vector((1.0, 1.0, 1.0)),
        )
    return Matrix.LocRotScale(
        hk_to_blender_loc(sample[0:3]),
        hk_to_blender_quat(sample[3:7], root_animation=False),
        Vector((1.0, 1.0, 1.0)),
    )


def get_matrix_map_global(
    obj,
    matrix_map_local,
    scale_map,
    reference_name=None,
    reference_base_local=None,
    reference_descendants=None,
):
    matrix_map_global = {}
    reference_descendants = reference_descendants or set()
    for pbone in obj.pose.bones:
        matrix = Matrix()
        scale = scale_map.get(pbone.name, Vector((1.0, 1.0, 1.0))).copy()
        ignore_reference_motion = pbone.name in reference_descendants

        for parent_bone in reversed(pbone.parent_recursive):
            if parent_bone.name in matrix_map_local:
                parent_matrix = matrix_map_local[parent_bone.name]
                matrix @= parent_matrix
                scale *= scale_map.get(parent_bone.name, Vector((1.0, 1.0, 1.0)))
        matrix @= matrix_map_local.get(pbone.name, Matrix())

        loc, rot, _ = matrix.decompose()
        matrix_map_global[pbone.name] = Matrix.LocRotScale(loc, rot, scale)
    return matrix_map_global


def set_pose_matrices_global(obj, matrix_map_global, frame):
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
        pbone.keyframe_insert("rotation_euler", frame=frame)
        pbone.keyframe_insert("scale", frame=frame)

        for child in pbone.children:
            rec(child, matrix)

    for pbone in obj.pose.bones:
        if not pbone.parent:
            rec(pbone, None)


def neutral_pose_bone(pb):
    pb.location = (0.0, 0.0, 0.0)
    pb.rotation_mode = "XYZ"
    pb.rotation_euler = (0.0, 0.0, 0.0)
    pb.scale = (1.0, 1.0, 1.0)


def key_neutral_pose_bone(pb, frame):
    neutral_pose_bone(pb)
    pb.keyframe_insert("location", frame=frame)
    pb.keyframe_insert("rotation_euler", frame=frame)
    pb.keyframe_insert("scale", frame=frame)


def child_object_matrices(arm_obj):
    saved = {}
    for obj in bpy.data.objects:
        if obj is arm_obj:
            continue
        if obj.parent is arm_obj:
            saved[obj.name] = obj.matrix_world.copy()
    return saved


def restore_child_object_matrices(saved):
    for name, matrix in saved.items():
        obj = bpy.data.objects.get(name)
        if obj:
            obj.matrix_world = matrix


def rename_animation_bones_from_names(track_names, arm_obj):
    wanted = [clean_bone_name(name) for name in track_names if clean_bone_name(name)]
    if not wanted:
        return

    used = {bone.name.lower() for bone in arm_obj.data.bones}
    for wanted_name in wanted:
        wanted_key = match_bone_name(wanted_name)
        if not wanted_key:
            continue

        for bone in arm_obj.data.bones:
            if match_bone_name(bone.name) != wanted_key:
                continue
            if bone.name == wanted_name:
                break
            if wanted_name.lower() in used:
                break

            old_name = bone.name
            bone.name = wanted_name
            used.discard(old_name.lower())
            used.add(wanted_name.lower())
            break


def apply_animation_handle_to_armature(dll, handle, arm_obj):
    if not arm_obj:
        raise HeatError("Select an armature before importing an animation")

    track_count = int(dll.HEAT_animation_track_count(handle))
    frame_total = max(1, int(dll.HEAT_animation_frame_count(handle)))
    fps = max(1, int(dll.HEAT_animation_fps(handle)))
    track_names = [
        (dll.HEAT_animation_track_name(handle, i) or b"").decode("utf-8", errors="replace")
        for i in range(track_count)
    ]

    rename_animation_bones_from_names(track_names, arm_obj)

    action_name = clean_bone_name(Path(bpy.context.scene.heat_last_animation_path).name) or "HavokAnimation"
    action = bpy.data.actions.new(action_name)
    arm_obj.animation_data_create()
    arm_obj.animation_data.action = action

    action[ACTION_FPS_PROP] = float(fps)
    if hasattr(action, "use_frame_range"):
        action.use_frame_range = True
        action.frame_start = 0
        action.frame_end = frame_total - 1

    scene = bpy.context.scene
    scene.frame_start = 0
    scene.frame_end = frame_total - 1
    scene.render.fps = fps

    saved_armature_world = arm_obj.matrix_world.copy()
    saved_children = child_object_matrices(arm_obj)
    current_frame = scene.frame_current

    bpy.ops.object.select_all(action="DESELECT")
    bpy.context.view_layer.objects.active = arm_obj
    arm_obj.select_set(True)
    bpy.ops.object.mode_set(mode="POSE")

    pose_bones = arm_obj.pose.bones
    bone_names = [bone.name for bone in arm_obj.data.bones]
    pose_lookup = build_bone_lookup(arm_obj)
    animated_names = set()

    for pb in pose_bones:
        neutral_pose_bone(pb)

    track_targets = []
    for track, track_name in enumerate(track_names):
        name = find_bone_name(track_name, track, bone_names, pose_lookup)
        if name not in pose_bones:
            continue
        track_targets.append((track, name))
        animated_names.add(name)

    reference_name = pose_lookup.get("reference")
    reference_descendants = set()
    reference_base_local = None
    if reference_name and reference_name in pose_bones:
        arm_obj.data.bones[reference_name].use_deform = False
        reference_descendants = {pb.name for pb in pose_bones[reference_name].children_recursive}

    sample_values = (ctypes.c_float * (track_count * 10))()
    for frame in range(frame_total):
        if not dll.HEAT_animation_sample_frame(handle, frame, sample_values, len(sample_values)):
            raise HeatError("Failed to read animation frame")

        matrix_map_local = {}
        scale_map = {}

        for pb in pose_bones:
            matrix_map_local[pb.name] = Matrix()
            scale_map[pb.name] = Vector((1.0, 1.0, 1.0))

        for track, name in track_targets:
            base = track * 10
            sample = sample_values[base:base + 10]
            pb = pose_bones[name]
            matrix_map_local[name] = sample_matrix_for_bone(sample, pb)
            scale_map[name] = sample_scale(sample)

        if reference_name and reference_name in matrix_map_local and reference_base_local is None:
            reference_base_local = matrix_map_local[reference_name].copy()

        matrix_map_global = get_matrix_map_global(
            arm_obj,
            matrix_map_local,
            scale_map,
            reference_name=reference_name,
            reference_base_local=reference_base_local,
            reference_descendants=reference_descendants,
        )
        set_pose_matrices_global(arm_obj, matrix_map_global, frame)

    last_frame = frame_total - 1
    for pb in pose_bones:
        if pb.name in animated_names:
            continue
        key_neutral_pose_bone(pb, 0)
        if last_frame:
            key_neutral_pose_bone(pb, last_frame)

    bpy.ops.object.mode_set(mode="OBJECT")
    arm_obj.matrix_world = saved_armature_world
    restore_child_object_matrices(saved_children)
    scene.frame_set(0)
    bpy.context.view_layer.update()
    scene.frame_set(current_frame if current_frame <= last_frame else 0)
    return action

def sanitize_text(value):
    return str(value).replace("\t", " ").replace("\r", " ").replace("\n", " ")


def matrix_to_native(matrix, is_root=False, root_motion=False):
    loc, rot, scale = matrix.decompose()
    if is_root:
        rot @= ROOT_EXPORT_CORRECTION
    if root_motion and is_root:
        loc_values = (loc.x, loc.y, loc.z)
    else:
        loc_values = (loc.y, loc.z, loc.x)
    return (
        loc_values[0],
        loc_values[1],
        loc_values[2],
        rot.y,
        rot.z,
        rot.x,
        rot.w,
        scale.y,
        scale.z,
        scale.x,
    )


def identity_native_values():
    return (
        0.0,
        0.0,
        0.0,
        0.0,
        0.0,
        0.0,
        1.0,
        1.0,
        1.0,
        1.0,
    )


def export_root_name(arm_obj):
    return clean_bone_name(arm_obj.name) or clean_bone_name(arm_obj.data.name) or "HavokSkeleton"


def export_bones(arm_obj, preserve_order=False):
    if preserve_order:
        bones = list(arm_obj.data.bones)
    else:
        bones = []

        def visit(bone):
            bones.append(bone)
            for child in sorted(bone.children, key=lambda item: item.name.lower()):
                visit(child)

        for root in sorted((bone for bone in arm_obj.data.bones if not bone.parent), key=lambda item: item.name.lower()):
            visit(root)

    entries = []
    index_by_name = {}

    for bone in bones:
        index_by_name[bone.name] = len(entries)
        entries.append(None)

    for bone in bones:
        if bone.parent:
            parent = index_by_name[bone.parent.name]
            local = bone.parent.matrix_local.inverted() @ bone.matrix_local
        else:
            parent = -1
            local = bone.matrix_local

        entries[index_by_name[bone.name]] = {
            "name": bone.name,
            "parent": parent,
            "matrix": local,
            "bone": bone,
            "root_correction": parent < 0,
        }

    return entries


def serialize_skeleton(arm_obj, preserve_order=False):
    entries = export_bones(arm_obj, preserve_order=preserve_order)
    lines = [f"skeleton\t{sanitize_text(export_root_name(arm_obj))}\t{len(entries)}"]

    for entry in entries:
        values = matrix_to_native(entry["matrix"], entry.get("root_correction", False))
        lines.append(
            "bone\t{}\t{}\t{}".format(
                entry["parent"],
                sanitize_text(clean_bone_name(entry["name"])),
                "\t".join(f"{v:.9g}" for v in values),
            )
        )

    return "\n".join(lines)


def pose_local_matrix(pose_bone):
    if pose_bone.parent:
        return pose_bone.parent.matrix.inverted() @ pose_bone.matrix
    return pose_bone.matrix.copy()


def action_has_curve(action, data_path, array_index=None):
    for fcurve in action.fcurves:
        if fcurve.data_path != data_path:
            continue
        if array_index is None or fcurve.array_index == array_index:
            return True
    return False


def root_motion_source_index(arm_obj, entries, action):
    root_key = match_bone_name(export_root_name(arm_obj))
    roots = [
        i for i, entry in enumerate(entries)
        if entry["bone"] is not None and entry["parent"] < 0
    ]
    if not roots:
        return None

    named_roots = [
        i for i in roots
        if match_bone_name(entries[i]["name"]) == root_key
    ]
    for i in named_roots + roots:
        data_path = f'pose.bones["{entries[i]["bone"].name}"].location'
        if action_has_curve(action, data_path):
            return i
    return None


def object_root_motion_sample(arm_obj, base_location):
    delta = arm_obj.location - base_location
    return (-delta.x, -delta.z, delta.y, 0.0)


def serialize_animation(arm_obj, pxd_main_tracks=False, preserve_bone_order=False):
    action = arm_obj.animation_data.action if arm_obj.animation_data else None
    if not action:
        raise HeatError("The selected armature has no active action")

    scene = bpy.context.scene
    source_fps = max(1.0, scene.render.fps / scene.render.fps_base)
    fps = EXPORT_ANIMATION_FPS
    start, end = action.frame_range
    start = int(math.floor(start))
    end = int(math.ceil(end))
    source_frames = max(0, end - start)
    duration = source_frames / source_fps
    frame_count = max(1, int(round(duration * fps)) + 1)

    entries = export_bones(arm_obj, preserve_order=preserve_bone_order)
    lines = [f"animation\t{fps}\t{duration:.9g}\t{frame_count}"]
    derived_root_motion_index = root_motion_source_index(arm_obj, entries, action)
    derive_object_root_motion = (
        derived_root_motion_index is None
        and action_has_curve(action, "location")
    )
    if derived_root_motion_index is not None or derive_object_root_motion:
        lines.append(
            "rootmotion\t{}\t{}\t{}".format(
                f"{duration:.9g}",
                "\t".join(f"{v:.9g}" for v in (0.0, 1.0, 0.0, 0.0)),
                "\t".join(f"{v:.9g}" for v in (0.0, 0.0, 1.0, 0.0)),
            )
        )
    for i, entry in enumerate(entries):
        lines.append(f"track\t{i}\t{sanitize_text(clean_bone_name(entry['name']))}")

    current_frame = scene.frame_current
    current_subframe = scene.frame_subframe
    base_object_location = arm_obj.location.copy()
    try:
        for out_frame in range(frame_count):
            if frame_count > 1:
                sample_frame = start + (source_frames * out_frame / (frame_count - 1))
            else:
                sample_frame = start
            sample_whole = int(math.floor(sample_frame))
            scene.frame_set(sample_whole, subframe=sample_frame - sample_whole)
            bpy.context.view_layer.update()
            derived_root_sample = None
            if derive_object_root_motion:
                derived_root_sample = object_root_motion_sample(arm_obj, base_object_location)
            for i, entry in enumerate(entries):
                bone = entry["bone"]
                if bone is None:
                    values = identity_native_values()
                else:
                    pb = arm_obj.pose.bones[bone.name]
                    values = matrix_to_native(
                        pose_local_matrix(pb),
                        entry.get("root_correction", False),
                        root_motion=not pxd_main_tracks,
                    )
                    if i == derived_root_motion_index:
                        derived_root_sample = (
                            -values[0],
                            -values[1],
                            -values[2],
                            0.0,
                        )
                lines.append(
                    "frame\t{}\t{}\t{}".format(
                        i, out_frame, "\t".join(f"{v:.9g}" for v in values)
                    )
                )
            if derived_root_sample is not None:
                lines.append(
                    "rootframe\t{}\t{}".format(
                        out_frame,
                        "\t".join(f"{float(v):.9g}" for v in derived_root_sample),
                    )
                )
    finally:
        scene.frame_set(current_frame, subframe=current_subframe)

    return "\n".join(lines)


def export_skeleton_native(filepath, skeleton_text, preset):
    dll = load_dll()
    ok = dll.HEAT_export_skeleton(
        path_bytes(filepath), skeleton_text.encode("utf-8"), int(preset)
    )
    if not ok:
        raise HeatError(last_error(dll))


def export_animation_native(filepath, skeleton_text, animation_text, preset):
    dll = load_dll()
    ok = dll.HEAT_export_animation(
        path_bytes(filepath),
        skeleton_text.encode("utf-8"),
        animation_text.encode("utf-8"),
        int(preset),
    )
    if not ok:
        raise HeatError(last_error(dll))


def export_compressed_animation_native(filepath, skeleton_text, animation_text, preset):
    dll = load_dll()
    export_func = dll.HEAT_export_compressed_animation
    export_func.argtypes = [
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_int,
    ]
    export_func.restype = ctypes.c_int
    ok = export_func(
        path_bytes(filepath),
        skeleton_text.encode("utf-8"),
        animation_text.encode("utf-8"),
        int(preset),
    )
    if not ok:
        raise HeatError(last_error(dll))


PRESETS = (
    ("0", "Generations", ""),
    ("1", "Unleashed", ""),
    ("2", "Lost World", ""),
    ("3", "Forces", ""),
)

GENERATIONS_PLATFORMS = (
    ("pc", "PC", ""),
    ("ps3", "PS3", ""),
    ("360", "360", ""),
)

UNLEASHED_PLATFORMS = (
    ("360", "360 / Recompiled", ""),
    ("ps3", "PS3", ""),
)

LOST_WORLD_PLATFORMS = (
    ("pc", "PC", ""),
    ("wiiu", "Wii U", ""),
)


def export_preset(game, generations="pc", unleashed="360", lost_world="pc"):
    if game == "3":
        return 7
    if game == "0":
        return {"pc": 0, "360": 2, "ps3": 3}.get(generations, 0)
    if game == "2":
        return {"pc": 5, "wiiu": 6}.get(lost_world, 5)
    return {"360": 1, "ps3": 4}.get(unleashed, 1)


def action_export_name(name):
    name = str(name or "")
    if "|" in name:
        name = name.rsplit("|", 1)[1]
    return name.strip()


def safe_action_filename(name):
    name = action_export_name(name)
    name = re.sub(r'[<>:"/\\|?*\x00-\x1f]+', "_", str(name or "")).strip(" .")
    return name or "Action"


def default_output_directory():
    if bpy.data.filepath:
        return os.path.dirname(bpy.data.filepath)
    return os.path.expanduser("~")


def action_export_filepath(context, extension):
    arm_obj = selected_armature()
    action = arm_obj.animation_data.action if arm_obj and arm_obj.animation_data else None
    name = safe_action_filename(action.name if action else "Animation")
    if not name.lower().endswith(extension.lower()):
        name += extension
    return os.path.join(default_output_directory(), name)


def export_hkx_animation_file(filepath, arm_obj, preset, compress=False):
    skeleton = serialize_skeleton(arm_obj)
    animation = serialize_animation(arm_obj)
    if compress:
        export_compressed_animation_native(filepath, skeleton, animation, preset)
    else:
        export_animation_native(filepath, skeleton, animation, preset)


class HEAT_OT_import_skeleton(bpy.types.Operator, ImportHelper):
    bl_idname = "heat.import_skeleton"
    bl_label = "Havok Skeleton (.skl.hkx)"
    bl_options = {"REGISTER", "UNDO"}
    filename_ext = ".hkx"
    filter_glob: StringProperty(default="*.skl.hkx;*.hkx", options={"HIDDEN"})

    def execute(self, context):
        try:
            dll = load_dll()
            handle = ctypes.c_void_p()
            if not dll.HEAT_open_skeleton(path_bytes(self.filepath), ctypes.byref(handle)):
                raise HeatError(last_error(dll))
            fallback_name = clean_file_name(Path(self.filepath).name)
            skeleton_name = (dll.HEAT_skeleton_name(handle) or b"").decode("utf-8", errors="replace")
            clean_name = clean_file_name(skeleton_name) or fallback_name or "HavokSkeleton"
            arm_obj = create_armature_from_skeleton_handle(dll, handle, clean_name)
            dll.HEAT_close_skeleton(handle)
            arm_obj.name = clean_name
            arm_obj.data.name = clean_name
            arm_obj[SKELETON_SOURCE_PROP] = self.filepath
        except Exception as ex:
            self.report({"ERROR"}, str(ex))
            return {"CANCELLED"}
        return {"FINISHED"}


class HEAT_OT_import_animation(bpy.types.Operator, ImportHelper):
    bl_idname = "heat.import_animation"
    bl_label = "Havok Animation (.anm.hkx)"
    bl_options = {"REGISTER", "UNDO"}
    filename_ext = ".hkx"
    filter_glob: StringProperty(default="*.anm.hkx;*.hkx", options={"HIDDEN"})
    def execute(self, context):
        try:
            context.scene.heat_last_animation_path = self.filepath
            arm_obj = selected_armature()
            if not arm_obj:
                raise HeatError("Select an armature before importing an animation")
            skeleton_path = arm_obj.get(SKELETON_SOURCE_PROP, "")
            if skeleton_path and not Path(skeleton_path).exists():
                skeleton_path = ""
            dll = load_dll()
            handle = ctypes.c_void_p()
            skeleton_arg = path_bytes(skeleton_path) if skeleton_path else None
            if not dll.HEAT_open_animation(
                path_bytes(self.filepath), skeleton_arg, ctypes.byref(handle)
            ):
                raise HeatError(last_error(dll))
            apply_animation_handle_to_armature(dll, handle, arm_obj)
            dll.HEAT_close_animation(handle)
        except Exception as ex:
            self.report({"ERROR"}, str(ex))
            return {"CANCELLED"}
        return {"FINISHED"}


class HEAT_OT_export_skeleton(bpy.types.Operator, ExportHelper):
    bl_idname = "heat.export_skeleton"
    bl_label = "Havok Skeleton (.skl.hkx)"
    filename_ext = ".skl.hkx"
    check_extension = False
    filter_glob: StringProperty(default="*.skl.hkx;*.hkx", options={"HIDDEN"})
    preset: EnumProperty(name="Game", items=PRESETS, default="1")
    generations: EnumProperty(name="Platform", items=GENERATIONS_PLATFORMS, default="pc")
    unleashed: EnumProperty(name="Platform", items=UNLEASHED_PLATFORMS, default="360")
    lost_world: EnumProperty(name="Platform", items=LOST_WORLD_PLATFORMS, default="pc")

    def draw(self, context):
        self.layout.prop(self, "preset")
        if self.preset == "0":
            self.layout.prop(self, "generations")
        elif self.preset == "1":
            self.layout.prop(self, "unleashed")
        elif self.preset == "2":
            self.layout.prop(self, "lost_world")

    def execute(self, context):
        try:
            arm_obj = selected_armature()
            if not arm_obj:
                raise HeatError("Select an armature before exporting a skeleton")
            preset = export_preset(self.preset, self.generations, self.unleashed, self.lost_world)
            export_skeleton_native(self.filepath, serialize_skeleton(arm_obj), preset)
        except Exception as ex:
            self.report({"ERROR"}, str(ex))
            return {"CANCELLED"}
        return {"FINISHED"}


class HEAT_OT_export_animation(bpy.types.Operator, ExportHelper):
    bl_idname = "heat.export_animation"
    bl_label = "Havok Animation (.anm.hkx)"
    filename_ext = ".anm.hkx"
    check_extension = False
    filter_glob: StringProperty(default="*.anm.hkx;*.hkx", options={"HIDDEN"})
    preset: EnumProperty(name="Game", items=PRESETS, default="1")
    generations: EnumProperty(name="Platform", items=GENERATIONS_PLATFORMS, default="pc")
    unleashed: EnumProperty(name="Platform", items=UNLEASHED_PLATFORMS, default="360")
    lost_world: EnumProperty(name="Platform", items=LOST_WORLD_PLATFORMS, default="pc")
    compress: BoolProperty(name="Compress Animation", default=False)

    def invoke(self, context, event):
        if not self.filepath:
            self.filepath = action_export_filepath(context, self.filename_ext)
        context.window_manager.fileselect_add(self)
        return {"RUNNING_MODAL"}

    def draw(self, context):
        self.layout.prop(self, "preset")
        if self.preset == "0":
            self.layout.prop(self, "generations")
        elif self.preset == "1":
            self.layout.prop(self, "unleashed")
        elif self.preset == "2":
            self.layout.prop(self, "lost_world")
        self.layout.prop(self, "compress")

    def execute(self, context):
        try:
            arm_obj = selected_armature()
            if not arm_obj:
                raise HeatError("Select an armature before exporting an animation")
            preset = export_preset(self.preset, self.generations, self.unleashed, self.lost_world)
            export_hkx_animation_file(self.filepath, arm_obj, preset, self.compress)
        except Exception as ex:
            self.report({"ERROR"}, str(ex))
            return {"CANCELLED"}
        return {"FINISHED"}


class HEAT_MT_import(bpy.types.Menu):
    bl_idname = "HEAT_MT_import"
    bl_label = "Hedgehog Engine Animation"

    def draw(self, context):
        self.layout.operator(HEAT_OT_import_skeleton.bl_idname, text="Havok Skeleton (.skl.hkx)")
        self.layout.operator(HEAT_OT_import_animation.bl_idname, text="Havok Animation (.anm.hkx)")
        self.layout.separator()
        pxd.draw_import_menu(self.layout)


class HEAT_MT_export(bpy.types.Menu):
    bl_idname = "HEAT_MT_export"
    bl_label = "Hedgehog Engine Animation"

    def draw(self, context):
        self.layout.operator(HEAT_OT_export_skeleton.bl_idname, text="Havok Skeleton (.skl.hkx)")
        self.layout.operator(HEAT_OT_export_animation.bl_idname, text="Havok Animation (.anm.hkx)")
        self.layout.separator()
        pxd.draw_export_menu(self.layout)


def import_menu(self, context):
    self.layout.menu(HEAT_MT_import.bl_idname)


def export_menu(self, context):
    self.layout.menu(HEAT_MT_export.bl_idname)


CLASSES = (
    HEAT_OT_import_skeleton,
    HEAT_OT_import_animation,
    HEAT_OT_export_skeleton,
    HEAT_OT_export_animation,
    HEAT_MT_import,
    HEAT_MT_export,
)

from . import pxd

# Batch exporting is not possible, and never will come to this addon.


def register():
    for cls in CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.TOPBAR_MT_file_import.append(import_menu)
    bpy.types.TOPBAR_MT_file_export.append(export_menu)
    bpy.types.Scene.heat_last_animation_path = StringProperty(options={"HIDDEN"}, default="")
    pxd.register()


def unregister():
    pxd.unregister()
    bpy.types.TOPBAR_MT_file_export.remove(export_menu)
    bpy.types.TOPBAR_MT_file_import.remove(import_menu)
    del bpy.types.Scene.heat_last_animation_path
    for cls in reversed(CLASSES):
        bpy.utils.unregister_class(cls)


if __name__ == "__main__":
    register()
