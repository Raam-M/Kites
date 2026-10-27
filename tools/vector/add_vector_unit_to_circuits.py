#!/usr/bin/env python3
"""Adds the vector unit to the circuit designs in resources/circuit_designs.

Each design gets a "Vector Unit" block and a "Vector Regs" block below the datapath, and
named wires that the processors highlight while a vector instruction executes:

    RF_to_VU, VU_to_RF      scalar operands and scalar results (x/f registers)
    VRF_to_VU, VU_to_VRF    vector register reads and writes
    VU_to_DM                vector loads and stores
    P4_to_VU                (pipelines) the instruction leaving MEM/WB, which executes in WB

Existing entries are left untouched; a design that already has the vector unit is skipped.

Usage: python3 -I tools/vector/add_vector_unit_to_circuits.py resources/circuit_designs
"""

import json
import os
import sys

NAMES = {"Vector Unit", "Vector Regs"}
WIRES = {"RF_to_VU", "VU_to_RF", "VRF_to_VU", "VU_to_VRF", "VU_to_DM", "P4_to_VU"}
ARROW = 5.196152422706632  # matches the arrow heads in the existing designs


def arrow(x, y, direction):
    if direction == "right":
        return [{"x": x, "y": y}, {"x": x - ARROW, "y": y + 3}, {"x": x - ARROW, "y": y - 3}]
    if direction == "left":
        return [{"x": x, "y": y}, {"x": x + ARROW, "y": y + 3}, {"x": x + ARROW, "y": y - 3}]
    if direction == "down":
        return [{"x": x, "y": y}, {"x": x - 3, "y": y - ARROW}, {"x": x + 3, "y": y - ARROW}]
    return [{"x": x, "y": y}, {"x": x - 3, "y": y + ARROW}, {"x": x + 3, "y": y + ARROW}]  # up


def wire(name, points, head):
    path = [{"type": 0 if i == 0 else 1, "x": x, "y": y} for i, (x, y) in enumerate(points)]
    x, y = points[-1]
    return {"arrowHeads": [arrow(x, y, head)], "junctions": [], "name": name, "path": path,
            "type": "WireItem"}


def bounds(design):
    ys = [c["y"] for c in design["components"]]
    xs = [c["x"] for c in design["components"]]
    for w in design["wires"]:
        for p in w["path"]:
            xs.append(p["x"])
            ys.append(p["y"])
    return min(xs), max(xs), min(ys), max(ys)


def component(design, name):
    return next(c for c in design["components"] if c["name"] == name)


def add_vector_unit(design):
    design["components"] = [c for c in design["components"] if c["name"] not in NAMES]
    design["wires"] = [w for w in design["wires"] if w["name"] not in WIRES]
    _, _, _, ymax = bounds(design)
    rf = component(design, "Reg File")      # 80 x 100 boxes
    dm = component(design, "Data Mem")
    regs = [c for c in design["components"] if c["type"] == "PipelineReg"]  # 20 x 500

    top = ymax + 50                          # Vector Unit: 80 x 100
    if regs:
        p4 = max(regs, key=lambda c: c["x"])
        vu_x = p4["x"] - 30
    else:
        vu_x = dm["x"] - 20
    vu = (vu_x, top)
    vrf = (vu_x - 160, top + 30)             # Vector Regs: HoriRect 80 x 40
    design["components"].append({"name": "Vector Unit", "type": "RectItem", "x": vu[0], "y": vu[1]})
    design["components"].append({"name": "Vector Regs", "type": "HoriRect", "x": vrf[0], "y": vrf[1]})

    wires = [
        wire("VRF_to_VU", [(vrf[0] + 80, vrf[1] + 12), (vu[0], vrf[1] + 12)], "right"),
        wire("VU_to_VRF", [(vu[0], vrf[1] + 28), (vrf[0] + 80, vrf[1] + 28)], "left"),
    ]
    # Memory: up from the vector unit into the bottom of the data memory.
    dm_x = dm["x"] + 60
    wires.append(wire("VU_to_DM", [(vu[0] + 20, vu[1]), (vu[0] + 20, top - 20), (dm_x, top - 20),
                                    (dm_x, dm["y"] + 100)], "up"))
    # Scalar operands: from the bottom of the register file down and across.
    rf_out = rf["x"] + 26  # left of the component under the register file
    wires.append(wire("RF_to_VU", [(rf_out, rf["y"] + 100), (rf_out, top + 115), (vu[0] + 20, top + 115),
                                    (vu[0] + 20, vu[1] + 100)], "up"))
    # Scalar results: back into the register file.
    rf_in = rf["x"] + 12
    wires.append(wire("VU_to_RF", [(vu[0] + 40, vu[1] + 100), (vu[0] + 40, top + 130), (rf_in, top + 130),
                                    (rf_in, rf["y"] + 100)], "up"))
    if regs:
        p4 = max(regs, key=lambda c: c["x"])
        # Clear of the wires that leave MEM/WB just to its right.
        wires.append(wire("P4_to_VU", [(p4["x"] + 20, p4["y"] + 480), (p4["x"] + 72, p4["y"] + 480),
                                        (p4["x"] + 72, top - 35), (vu[0] + 70, top - 35),
                                        (vu[0] + 70, vu[1])], "down"))
    design["wires"].extend(wires)


def array_end(text, key):
    """Index of the closing bracket of the top-level array `key`."""
    start = text.index(f'"{key}"')
    i = text.index("[", start)
    depth, in_string = 0, False
    while True:
        c = text[i]
        if in_string:
            if c == "\\":
                i += 1
            elif c == '"':
                in_string = False
        elif c == '"':
            in_string = True
        elif c in "[{":
            depth += 1
        elif c in "]}":
            depth -= 1
            if depth == 0:
                return i
        i += 1


def insert_items(text, key, items):
    """Appends items to a top-level array without reformatting the existing entries."""
    end = array_end(text, key)
    body = text[:end].rstrip()
    blocks = []
    for item in items:
        dumped = json.dumps(item, indent=4)
        blocks.append("\n".join("        " + line for line in dumped.splitlines()))
    separator = "," if not body.endswith("[") else ""
    return body + separator + "\n" + ",\n".join(blocks) + "\n    " + text[end:]


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    folder = sys.argv[1]
    for name in ["single_cycle_draft1.json", "NH_NF_Processor.json", "H_NF_Processor.json",
                 "NH_F_Processor.json", "H_F_Processor.json"]:
        path = os.path.join(folder, name)
        text = open(path).read()
        design = json.loads(text)
        if any(c["name"] in NAMES for c in design["components"]):
            print("already has a vector unit:", path)
            continue
        before_components = len(design["components"])
        before_wires = len(design["wires"])
        add_vector_unit(design)
        text = insert_items(text, "components", design["components"][before_components:])
        text = insert_items(text, "wires", design["wires"][before_wires:])
        json.loads(text)  # still valid JSON
        with open(path, "w") as f:
            f.write(text)
        print("updated", path)


if __name__ == "__main__":
    main()
