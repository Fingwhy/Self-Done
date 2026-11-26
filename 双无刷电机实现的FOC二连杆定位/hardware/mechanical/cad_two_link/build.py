"""Build the short equal-length desktop arm in SolidWorks 2022."""
from collections import Counter
import json
import math
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT.parent / "cad_gripper"))
import cad
from cad import NULL, circle, extrude, new, poly, read, sketch, sw, volume
import pythoncom
import win32com.client

PARTS = ROOT / "SolidWorks"
PRINT = ROOT / "STL"
ASSEMBLY_TEMPLATE = r"C:\ProgramData\SOLIDWORKS\SOLIDWORKS 2022\templates\gb_assembly.asmdot"
ASSEMBLY_NAME = "双电机平面二连杆"
LENGTH = 100.0
BASE_T = 9.0
ARM_T = 8.0
TIP_T = 6.0
SHOULDER_H = 33.8
ELBOW_H = 45.75
SHOULDER_Z = BASE_T + SHOULDER_H
ARM_Z = SHOULDER_Z
ELBOW_Z = SHOULDER_Z + ARM_T
TIP_Z = ELBOW_Z + ELBOW_H
BASE = "01_底座_MS7015"
ARM = "02_大臂_100mm"
TIP = "03_小臂_100mm"
MOTOR1 = "参考_MS7015_外形"
MOTOR2 = "参考_MS7025_外形"
POINTER = "参考_指针"
BASE_OUTLINE = [
    (-55, -44), (-49, -50), (49, -50), (55, -44), (55, 44),
    (49, 50), (-49, 50), (-55, 44), (-55, 10), (12, 10),
    (12, -10), (-55, -10),
]
ARM_OUTLINE = [
    (-24, -20), (-20, -24), (21, -24), (37, -14), (63, -14),
    (79, -24), (120, -24), (124, -20), (124, 20), (120, 24),
    (79, 24), (63, 14), (37, 14), (21, 24), (-20, 24), (-24, 20),
    (-24, 5), (8, 5), (8, -5), (-24, -5),
]
TIP_OUTLINE = [
    (-22, -17), (-17, -22), (21, -22), (39, -10), (99, -8),
    (108, -5), (108, 5), (99, 8), (39, 10), (21, 22),
    (-17, 22), (-22, 17),
]


def hole_pattern(x, pitch, diameter):
    half = pitch / 2
    return [(x + dx, dy, diameter / 2) for dx in (-half, half) for dy in (-half, half)]


def area(points):
    return abs(sum(x * v - y * u for (x, y), (u, v) in
                   zip(points, points[1:] + points[:1]))) / 2


def box(doc):
    return [round(value * 1000, 6) for value in doc.GetBodies2(0, False)[0].GetBodyBox()]


def fingerprint(doc):
    features = []
    feature = read(doc, "FirstFeature")
    while feature is not None:
        features.append(read(feature, "Name"))
        feature = read(feature, "GetNextFeature")
    bodies = doc.GetBodies2(0, False) or []
    return {"features": features, "body_count": len(bodies),
            "box_mm": box(doc) if bodies else [], "volume_mm3": volume(doc)}


def mesh_check(path, expected_box, expected_volume):
    data = path.read_bytes()
    count = struct.unpack_from("<I", data, 80)[0]
    if len(data) == 84 + 50 * count:
        points = [values[3:12] for values in struct.iter_unpack("<12fH", data[84:])]
    else:
        vertices = [tuple(map(float, line.split()[1:])) for line in
                    data.decode("ascii").splitlines() if line.strip().startswith("vertex ")]
        points = [sum(vertices[i:i + 3], ()) for i in range(0, len(vertices), 3)]
    assert points, path
    limits = [(float("inf"), float("-inf")) for _ in range(3)]
    signed_volume = 0.0
    edges = Counter()
    for row in points:
        a, b, c = (tuple(row[i:i + 3]) for i in (0, 3, 6))
        signed_volume += (
            a[0] * (b[1] * c[2] - b[2] * c[1])
            + a[1] * (b[2] * c[0] - b[0] * c[2])
            + a[2] * (b[0] * c[1] - b[1] * c[0])
        ) / 6
        rounded = [tuple(round(v, 5) for v in p) for p in (a, b, c)]
        for first, second in zip(rounded, rounded[1:] + rounded[:1]):
            edges[tuple(sorted((first, second)))] += 1
        for i, (lo, hi) in enumerate(limits):
            limits[i] = min(lo, a[i], b[i], c[i]), max(hi, a[i], b[i], c[i])
    extents = [hi - lo for lo, hi in limits]
    expected_extents = [hi - lo for lo, hi in zip(expected_box[:3], expected_box[3:])]
    assert all(abs(a - b) < .08 for a, b in zip(extents, expected_extents)), (path, extents)
    assert math.isclose(abs(signed_volume), expected_volume, rel_tol=.01), (path, signed_volume)
    assert all(n == 2 for n in edges.values()), (path, "mesh not watertight")
    return {"triangles": len(points), "size_mm": extents, "volume_mm3": abs(signed_volume)}


def build_part(name, height, outline=None, holes=(), radius=None, recesses=(), recess_depth=0):
    doc = new()
    try:
        expected_volume = 0

        def layer(z, depth, contours, label):
            nonlocal expected_volume
            before = fingerprint(doc)
            sketch(doc)
            if outline:
                poly(doc, outline)
            else:
                circle(doc, 0, 0, radius)
            for x, y, r in contours:
                circle(doc, x, y, r)
            extrude(doc, z, depth, label)
            expected_volume += ((area(outline) if outline else math.pi * radius ** 2)
                                - sum(math.pi * r ** 2 for x, y, r in contours)) * depth
            after = fingerprint(doc)
            assert after["body_count"] == 1 and len(after["features"]) > len(before["features"])
            assert math.isclose(after["volume_mm3"], expected_volume, rel_tol=1e-6)
            return after

        assert fingerprint(doc)["body_count"] == 0
        if recesses:
            layer(0, recess_depth, recesses + holes[len(recesses):], "底层与螺钉头避空")
            after = layer(recess_depth, height - recess_depth, holes, "上层与通孔")
        else:
            after = layer(0, height, holes, "板体与通孔" if outline else "外形")
        expected_box = ([min(p[0] for p in outline), min(p[1] for p in outline), 0,
                         max(p[0] for p in outline), max(p[1] for p in outline), height]
                        if outline else [-radius, -radius, 0, radius, radius, height])
        assert all(abs(a - b) < .02 for a, b in zip(after["box_mm"], expected_box)), after
        doc.ShowNamedView2("*Isometric", 7)
        doc.ViewZoomtofit2()
        save(doc, PARTS / f"{name}.SLDPRT")
        if outline:
            path = PRINT / f"{name}.STL"
            save(doc, path)
            after["stl"] = mesh_check(path, expected_box, expected_volume)
        print(name, after["box_mm"], flush=True)
        return after
    except Exception:
        print(f"{name}: {fingerprint(doc)}", flush=True)
        raise
    finally:
        sw.CloseDoc(read(doc, "GetTitle"))


def save(doc, path):
    result = doc.SaveAs3(str(path), 0, 2)
    assert result == 0 and path.is_file() and path.stat().st_size > 0, (path, result)


def transform(angle, x, y, z):
    c, s = math.cos(angle), math.sin(angle)
    # The parts print flat in XY. SolidWorks' assembly top view looks down Y.
    return [c, 0., -s, -s, 0., -c, 0., 1., 0.,
            x / 1000, z / 1000, -y / 1000, 1., 0., 0., 0.]


def build_assembly():
    documents = []
    assembly = None
    names = [BASE, MOTOR1, ARM, MOTOR2, TIP, POINTER]
    error = win32com.client.VARIANT(pythoncom.VT_BYREF | pythoncom.VT_I4, 0)
    warning = win32com.client.VARIANT(pythoncom.VT_BYREF | pythoncom.VT_I4, 0)
    try:
        for name in names:
            path = str(PARTS / f"{name}.SLDPRT")
            part = sw.OpenDoc6(path, 1, 1, "", error, warning)
            assert part is not None, (path, error.value)
            documents.append(part)
        assembly = sw.NewDocument(ASSEMBLY_TEMPLATE, 0, 0, 0)
        assert assembly is not None and assembly.GetComponentCount(False) == 0
        math_utility = read(sw, "GetMathUtility")
        math_utility._FlagAsMethod("CreateTransform")
        components = {}
        for count, name in enumerate(names, 1):
            component = assembly.AddComponent5(str(PARTS / f"{name}.SLDPRT"), 0, "", False, "", 0, 0, 0)
            assert component is not None, name
            components[name] = component
            assembly.ForceRebuild3(False)
            assert assembly.GetComponentCount(False) == count

        def move(name, values):
            component = components[name]
            assembly.ClearSelection2(True)
            assert component.Select4(False, NULL, False), name
            assembly.UnfixComponent()
            component.Transform2 = math_utility.CreateTransform(
                win32com.client.VARIANT(pythoncom.VT_ARRAY | pythoncom.VT_R8, values))
            actual = read(read(component, "Transform2"), "ArrayData")
            assert all(abs(a - b) < 1e-9 for a, b in zip(actual[:13], values[:13])), name
            assembly.ClearSelection2(True)
            assert component.Select4(False, NULL, False), name
            assembly.FixComponent()

        def pose(q1, q2):
            a, b = math.radians(q1), math.radians(q1 + q2)
            x, y = LENGTH * math.cos(a), LENGTH * math.sin(a)
            tx, ty = x + LENGTH * math.cos(b), y + LENGTH * math.sin(b)
            move(BASE, transform(0, 0, 0, 0))
            move(MOTOR1, transform(0, 0, 0, BASE_T))
            move(ARM, transform(a, 0, 0, ARM_Z))
            move(MOTOR2, transform(a, x, y, ELBOW_Z))
            move(TIP, transform(b, x, y, TIP_Z))
            move(POINTER, transform(0, tx, ty, 2))
            assembly.ClearSelection2(True)
            assembly.ForceRebuild3(False)
            assert assembly.GetComponentCount(False) == len(names)
            manager = read(assembly, "InterferenceDetectionManager")
            manager.TreatCoincidenceAsInterference = False
            manager._FlagAsMethod("GetInterferences")
            try:
                for interference in manager.GetInterferences() or []:
                    overlap = read(interference, "Volume") * 1e9
                    if overlap > 1e-5:
                        parts = [read(c, "Name2") for c in read(interference, "Components")]
                        raise AssertionError((q1, q2, parts, overlap))
            finally:
                manager.Done()
            return {"shoulder_deg": q1, "elbow_deg": q2, "tip_xy_mm": [tx, ty]}

        checks = [pose(q1, q2) for q1 in (-60, 0, 60) for q2 in (-130, -90, 0, 90, 130)]
        pose(0, 75)
        assembly.ShowNamedView2("*Isometric", 7)
        assembly.ViewZoomtofit2()
        save(assembly, PARTS / f"{ASSEMBLY_NAME}.SLDASM")
        for view, index, label in (("*Isometric", 7, "总装预览"), ("*Top", 5, "俯视预览")):
            assembly.ShowNamedView2(view, index)
            assembly.ViewZoomtofit2()
            path = ROOT / f"{label}.bmp"
            assembly.SaveBMP(str(path), 1400, 1050)
            assert path.is_file() and path.stat().st_size > 0
        return checks
    finally:
        if assembly is not None:
            sw.CloseDoc(read(assembly, "GetTitle"))
        for part in reversed(documents):
            sw.CloseDoc(read(part, "GetTitle"))


def main():
    PARTS.mkdir(exist_ok=True)
    PRINT.mkdir(exist_ok=True)
    report_path = ROOT / "验证记录.json"
    if sys.argv[1:] == ["--assembly-only"]:
        report = json.loads(report_path.read_text(encoding="utf-8"))
        report["assembly_poses_no_interference"] = build_assembly()
        report_path.write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
        print("PASS: assembly rebuilt, 15 sampled poses", flush=True)
        return
    bottom = hole_pattern(0, 29.69, 3.4)
    top = hole_pattern(0, 17.7, 4.5)
    desk = [(x, y, 2.75) for x in (-44, 44) for y in (-39, 39)]
    report = {}
    report[BASE] = build_part(BASE, BASE_T, BASE_OUTLINE, bottom + desk,
                              recesses=[(x, y, 3.75) for x, y, _ in bottom], recess_depth=4)
    report[ARM] = build_part(ARM, ARM_T, ARM_OUTLINE, top + hole_pattern(LENGTH, 29.69, 3.4))
    report[TIP] = build_part(TIP, TIP_T, TIP_OUTLINE, top + [(LENGTH, 0, 1.7)])
    report[MOTOR1] = build_part(MOTOR1, SHOULDER_H, radius=35)
    report[MOTOR2] = build_part(MOTOR2, ELBOW_H, radius=35)
    report[POINTER] = build_part(POINTER, TIP_Z - 2, radius=1.5)
    report["assembly_poses_no_interference"] = build_assembly()
    report_path.write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
    print("PASS: 3 print parts, 6 assembly components, 15 sampled poses", flush=True)


if __name__ == "__main__":
    main()
