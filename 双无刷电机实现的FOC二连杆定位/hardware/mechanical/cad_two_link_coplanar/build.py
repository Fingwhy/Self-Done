"""Create the equal-height motor layout in SolidWorks."""
import json
import math
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT.parent / "cad_two_link"))
import build as cad_build

cad_build.ROOT = ROOT
cad_build.PARTS = ROOT / "SolidWorks"
cad_build.PRINT = ROOT / "STL"
cad_build.BASE = "01_底座_MS7015_同平面"
cad_build.ARM = "02_下沉大臂_100mm"
cad_build.TIP = "03_小臂_100mm_同平面"
cad_build.MOTOR1 = "同平面参考_MS7015_外形"
cad_build.MOTOR2 = "同平面参考_MS7025_外形"
cad_build.POINTER = "同平面参考_指针"
cad_build.ASSEMBLY_NAME = "双电机平面二连杆_底面等高"
cad_build.ARM_Z = 0.0
cad_build.ELBOW_Z = cad_build.BASE_T
cad_build.TIP_Z = cad_build.ELBOW_Z + cad_build.ELBOW_H

FIXED_RADIUS = 46.0
SLOT_HALF = 10.0
ARC_LIMIT = math.pi - math.asin(SLOT_HALF / FIXED_RADIUS)
BASE_OUTLINE = [
    (FIXED_RADIUS * math.cos(angle), FIXED_RADIUS * math.sin(angle))
    for angle in (-ARC_LIMIT + 2 * ARC_LIMIT * i / 96 for i in range(97))
] + [(12, SLOT_HALF), (12, -SLOT_HALF)]
FOOT = [(48, -24), (120, -24), (125, -19), (125, 19),
        (120, 24), (48, 24)]
WEB = [(48, -24), (61, -24), (61, 24), (48, 24)]
UPPER = [
    (-24, -20), (-20, -24), (61, -24), (61, 24), (-20, 24), (-24, 20),
    (-24, 5), (8, 5), (8, -5), (-24, -5),
]


def build_dropped_arm():
    """Start at the moving foot and merge the vertical web and shoulder flange."""
    doc = cad_build.new()
    expected_volume = 0.0
    shoulder_z = cad_build.SHOULDER_Z
    bottom = cad_build.hole_pattern(cad_build.LENGTH, 29.69, 3.4)
    counterbores = [(x, y, 3.75) for x, y, _ in bottom]
    top = cad_build.hole_pattern(0, 17.7, 4.5)
    layers = [
        (0, 4, FOOT, counterbores, "肘部托板底层和螺钉头避空"),
        (4, 5, FOOT, bottom, "肘部托板顶层与安装孔"),
        (9, shoulder_z - 9, WEB, [], "下沉立壁"),
        (shoulder_z, cad_build.ARM_T, UPPER, top, "肩端连接板与穿线槽"),
    ]
    try:
        for start, depth, outline, holes, label in layers:
            before = cad_build.fingerprint(doc)
            cad_build.sketch(doc)
            cad_build.poly(doc, outline)
            for x, y, radius in holes:
                cad_build.circle(doc, x, y, radius)
            cad_build.extrude(doc, start, depth, label)
            expected_volume += (cad_build.area(outline) - sum(
                math.pi * radius ** 2 for _, _, radius in holes)) * depth
            after = cad_build.fingerprint(doc)
            assert after["body_count"] == 1
            assert len(after["features"]) > len(before["features"])
            assert math.isclose(after["volume_mm3"], expected_volume, rel_tol=1e-6)
        expected_box = [-24, -24, 0, 125, 24, shoulder_z + cad_build.ARM_T]
        assert all(abs(a - b) < .02 for a, b in zip(after["box_mm"], expected_box)), after
        doc.ShowNamedView2("*Isometric", 7)
        doc.ViewZoomtofit2()
        cad_build.save(doc, cad_build.PARTS / f"{cad_build.ARM}.SLDPRT")
        mesh = cad_build.PRINT / f"{cad_build.ARM}.STL"
        cad_build.save(doc, mesh)
        after["stl"] = cad_build.mesh_check(mesh, expected_box, expected_volume)
        return after
    except Exception:
        print(cad_build.fingerprint(doc), flush=True)
        raise
    finally:
        cad_build.sw.CloseDoc(cad_build.read(doc, "GetTitle"))


def main():
    cad_build.PARTS.mkdir(exist_ok=True)
    cad_build.PRINT.mkdir(exist_ok=True)
    report_path = ROOT / "验证记录.json"
    if sys.argv[1:] == ["--assembly-only"]:
        report = json.loads(report_path.read_text(encoding="utf-8"))
        finish_assembly(report, report_path)
        return
    bottom = cad_build.hole_pattern(0, 29.69, 3.4)
    top = cad_build.hole_pattern(0, 17.7, 4.5)
    desk = [(0, -40.5, 2.75), (0, 40.5, 2.75), (40.5, 0, 2.75)]
    report = {
        cad_build.BASE: cad_build.build_part(
            cad_build.BASE, cad_build.BASE_T, BASE_OUTLINE, bottom + desk,
            recesses=[(x, y, 3.75) for x, y, _ in bottom], recess_depth=4),
        cad_build.ARM: build_dropped_arm(),
        cad_build.TIP: cad_build.build_part(
            cad_build.TIP, cad_build.TIP_T, cad_build.TIP_OUTLINE,
            top + [(cad_build.LENGTH, 0, 1.7)]),
        cad_build.MOTOR1: cad_build.build_part(cad_build.MOTOR1, cad_build.SHOULDER_H, radius=35),
        cad_build.MOTOR2: cad_build.build_part(cad_build.MOTOR2, cad_build.ELBOW_H, radius=35),
        cad_build.POINTER: cad_build.build_part(cad_build.POINTER, cad_build.TIP_Z - 2, radius=1.5),
    }
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    finish_assembly(report, report_path)


def finish_assembly(report, report_path):
    report["assembly_poses_no_interference"] = cad_build.build_assembly()
    report["motor_mounting_plane_height_mm"] = {"MS7015": cad_build.BASE_T, "MS7025": cad_build.ELBOW_Z}
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print("PASS: 3 printable parts; coplanar motor bottoms; 15 interference checks", flush=True)


if __name__ == "__main__":
    main()
