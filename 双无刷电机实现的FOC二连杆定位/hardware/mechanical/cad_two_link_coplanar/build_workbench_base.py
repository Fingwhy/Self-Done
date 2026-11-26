"""Add two workbench mounting ears to the saved coplanar SolidWorks base."""

import json
import math
from pathlib import Path
from runpy import run_path
import shutil

from PIL import Image
from shapely.geometry import Polygon

ROOT = Path(__file__).resolve().parent
coplanar = run_path(str(ROOT / "build.py"))
cad = coplanar["cad_build"]
BASE_OUTLINE = coplanar["BASE_OUTLINE"]
PITCH = 125.33
HOLE_DIAMETER = 5.8
ROOT_DISTANCE = 34.0
ROOT_HALF_WIDTH = 13.0
END_RADIUS = 9.0
OUTPUT = ROOT / "工作台固定版"
NATIVE = OUTPUT / "SolidWorks"
BASE_NAME = "01_底座_MS7015_工作台125.33双耳"
ASSEMBLY_NAME = "双电机平面二连杆_工作台固定版"


def sketch_xyz(point):
    return point[0] / 1000, point[1] / 1000, 0.0


def add_ear(doc, center, label):
    """Merge a tapered boss with a semicircular end and a through hole."""
    distance = math.hypot(*center)
    ux, uy = (coordinate / distance for coordinate in center)

    def xy(radial, transverse):
        return ux * radial - uy * transverse, uy * radial + ux * transverse

    root_left = xy(ROOT_DISTANCE, -ROOT_HALF_WIDTH)
    root_right = xy(ROOT_DISTANCE, ROOT_HALF_WIDTH)
    end_left = xy(distance, -END_RADIUS)
    end_right = xy(distance, END_RADIUS)
    end_tip = xy(distance + END_RADIUS, 0)

    # Independent area calculation using a fine polygon for the semicircle.
    cap = [xy(distance + END_RADIUS * math.cos(angle), END_RADIUS * math.sin(angle))
           for angle in (-math.pi / 2 + math.pi * i / 512 for i in range(513))]
    profile = Polygon([root_left, *cap, root_right])
    added_area = profile.difference(Polygon(BASE_OUTLINE)).area
    expected_delta = cad.BASE_T * (added_area - math.pi * (HOLE_DIAMETER / 2) ** 2)

    before = cad.fingerprint(doc)
    cad.sketch(doc)
    sketch = doc.SketchManager
    for start, end in ((root_left, end_left), (end_right, root_right), (root_right, root_left)):
        assert sketch.CreateLine(*sketch_xyz(start), *sketch_xyz(end)) is not None
    assert sketch.Create3PointArc(*sketch_xyz(end_left), *sketch_xyz(end_right),
                                  *sketch_xyz(end_tip)) is not None
    cad.circle(doc, *center, HOLE_DIAMETER / 2)
    cad.extrude(doc, 0, cad.BASE_T, label)
    after = cad.fingerprint(doc)
    assert after["body_count"] == 1 and label in after["features"], after
    delta = after["volume_mm3"] - before["volume_mm3"]
    assert math.isclose(delta, expected_delta, abs_tol=0.03), (label, delta, expected_delta)


def verify_holes(doc, centers):
    holes = []
    for face in cad.read(doc.GetBodies2(0, False)[0], "GetFaces"):
        surface = cad.read(face, "GetSurface")
        if not cad.read(surface, "IsCylinder"):
            continue
        x, y, _, dx, dy, dz, radius = cad.read(surface, "CylinderParams")
        if math.isclose(radius * 2000, HOLE_DIAMETER, abs_tol=1e-6):
            assert abs(dx) < 1e-8 and abs(dy) < 1e-8 and abs(abs(dz) - 1) < 1e-8
            bounds = [v * 1000 for v in cad.read(face, "GetBox")]
            assert abs(bounds[2]) < 0.01 and abs(bounds[5] - cad.BASE_T) < 0.01
            holes.append((round(x * 1000, 6), round(y * 1000, 6)))
    assert sorted(holes) == sorted(centers), holes
    return holes


def export_base(source):
    native = NATIVE / f"{BASE_NAME}.SLDPRT"
    assert cad.sw.GetOpenDocumentByName(str(native)) is None, native
    source_data = source.read_bytes()
    native.write_bytes(source_data)
    err = cad.win32com.client.VARIANT(cad.pythoncom.VT_BYREF | cad.pythoncom.VT_I4, 0)
    warning = cad.win32com.client.VARIANT(cad.pythoncom.VT_BYREF | cad.pythoncom.VT_I4, 0)
    doc = cad.sw.OpenDoc6(str(native), 1, 1, "", err, warning)
    assert doc is not None, (native, err.value)
    assert Path(cad.read(doc, "GetPathName")).resolve() == native.resolve()
    try:
        doc.ForceRebuild3(False)
        before = cad.fingerprint(doc)
        assert before["body_count"] == 1 and abs(before["box_mm"][5] - cad.BASE_T) < 0.01
        half = PITCH / 2
        centers = [(-half, -half), (-half, half)]
        for center, side in zip(centers, ("左", "右")):
            add_ear(doc, center, f"{side}工作台安装耳_通孔{HOLE_DIAMETER:g}")
        after = cad.fingerprint(doc)
        expected_box = [-half - END_RADIUS, -half - END_RADIUS, 0,
                        before["box_mm"][3], half + END_RADIUS, cad.BASE_T]
        assert all(abs(a - b) < 0.02 for a, b in zip(after["box_mm"], expected_box)), after
        after["bench_hole_centers_xy_mm"] = verify_holes(doc, centers)
        after["bench_hole_pitch_mm"] = math.dist(*centers)
        after["bench_hole_diameter_mm"] = HOLE_DIAMETER
        doc.ShowNamedView2("*Isometric", 7)
        doc.ViewZoomtofit2()
        cad.save(doc, native)
        mesh = OUTPUT / f"{BASE_NAME}.STL"
        cad.save(doc, mesh)
        after["stl"] = cad.mesh_check(mesh, expected_box, after["volume_mm3"])
        assert source.read_bytes() == source_data
        return {"source": before, "workbench_base": after}
    except Exception:
        print(cad.fingerprint(doc), flush=True)
        raise
    finally:
        cad.sw.CloseDoc(cad.read(doc, "GetTitle"))


def main():
    NATIVE.mkdir(parents=True, exist_ok=True)
    originals = cad.PARTS
    report = export_base(originals / f"{cad.BASE}.SLDPRT")
    for key in ("ARM", "TIP", "MOTOR1", "MOTOR2", "POINTER"):
        name = getattr(cad, key)
        source = originals / f"{name}.SLDPRT"
        target = NATIVE / f"工作台版_{source.name}"
        assert cad.sw.GetOpenDocumentByName(str(target)) is None, target
        shutil.copyfile(source, target)
        setattr(cad, key, target.stem)
    cad.PARTS = NATIVE
    cad.BASE = BASE_NAME
    cad.ROOT = OUTPUT
    cad.ASSEMBLY_NAME = ASSEMBLY_NAME
    assert cad.sw.GetOpenDocumentByName(str(NATIVE / f"{ASSEMBLY_NAME}.SLDASM")) is None
    report["assembly_poses_no_interference"] = cad.build_assembly()
    report["motor_mounting_plane_height_mm"] = {"MS7015": cad.BASE_T, "MS7025": cad.ELBOW_Z}
    for label in ("总装预览", "俯视预览"):
        bmp = OUTPUT / f"{label}.bmp"
        with Image.open(bmp) as image:
            image.save(OUTPUT / f"{label}.png")
        bmp.unlink()
    (OUTPUT / "验证记录.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print("PASS: workbench base, printable STL and 15 assembly poses", flush=True)


if __name__ == "__main__":
    main()
