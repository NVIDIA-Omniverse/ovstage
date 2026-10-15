# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: LicenseRef-NvidiaProprietary
"""Regenerate overview SVG timelines: python3 generate.py (standard library only)."""
from pathlib import Path
from html import escape

ROOT = Path(__file__).resolve().parent
RED = "#c43c39"
GREEN = "#76b900"
LEFT = 156
STEP = 83
WIDTH = 800
ORDINALS = range(8, 15)


def text(x, y, value, size=14, color="#18232b", anchor="start", weight="normal"):
    return f'<text x="{x}" y="{y}" font-size="{size}" fill="{color}" text-anchor="{anchor}" font-weight="{weight}">{escape(str(value))}</text>'


def rect(x, y, w, h, fill, radius=0):
    return f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{radius}" fill="{fill}"/>'


def panel(top, title, rows, marks, changes, note):
    out = [text(24, top+24, title, 18, weight="bold")]
    axis = top+65
    bottom = axis + 38 + 58*len(rows)
    for i, ordinal in enumerate(ORDINALS):
        x = LEFT + STEP*i
        out += [text(x+STEP/2, axis-12, ordinal, 13, anchor="middle"),
                f'<path d="M{x},{axis} V{bottom}" stroke="#dce2e7" stroke-dasharray="3 4"/>']
    out.append(text(24, axis-12, "Ordinal →", 13))
    for j, row in enumerate(rows):
        y = axis+12+j*58
        mark = marks[j]
        bound = LEFT+STEP*(mark-7)
        out += [text(24, y+27, row, 15, weight="bold"),
                rect(LEFT,y,bound-LEFT,38,"#fbe9e8"),
                rect(bound,y,LEFT+STEP*7-bound,38,"#edf6de")]
        for ordinal, label in changes[j]:
            x = LEFT+STEP*(ordinal-8)+9
            out += [rect(x,y+4,STEP-18,30,RED if ordinal<=mark else GREEN,3),
                    text(x+(STEP-18)/2,y+25,label,12,"white" if ordinal<=mark else "#152300","middle","bold")]
        out += [f'<path d="M{bound},{y-4} V{y+43}" stroke="#18232b" stroke-width="2.5"/>',
                text(bound+5,y+55,f"W = {mark}",11)]
    out.append(text(24,bottom+30,note,13))
    return out, bottom+50


def save(name,title,panels):
    out=[]
    top=12
    for args in panels:
        items, end = panel(top,*args)
        out.extend(items)
        top=end+12
    out += [rect(24,top,16,16,RED,2),text(48,top+13,"Immutable through watermark",13),
            rect(410,top,16,16,GREEN,2),text(434,top+13,"Beyond watermark: writes allowed*",13),
            text(24,top+43,"Each rectangle is a change at one ordinal; gaps mean no change shown.",12),
            text(24,top+64,"*Admission policy still applies; NONDECREASING allows equal unsealed ordinals.",12)]
    height=top+88
    svg=f'''<svg xmlns="http://www.w3.org/2000/svg" width="800" height="{height}" viewBox="0 0 800 {height}" role="img" aria-labelledby="title">
<title id="title">{escape(title)}</title>
<!-- SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &amp; AFFILIATES. All rights reserved. -->
<!-- SPDX-License-Identifier: LicenseRef-NvidiaProprietary -->
<rect width="800" height="{height}" fill="white"/>
<g font-family="Arial, Helvetica, sans-serif">{''.join(out)}</g></svg>'''
    (ROOT / (name+".svg")).write_text(svg, encoding="utf-8")


save("data-model", "Attribute changes along an ordinal axis", [
    ("Changes on /World/BodyA", ["mass", "velocities"], [9,9],
     [[(8,"2.0"),(10,"3.0")],[(8,"v₁"),(9,"v₂"),(10,"v₃")]],
     "Both watermarks are 9. Changes at 10 are beyond the watermark.")])
save("write-read", "Advancing the watermark from 9 to 10", [
    ("1. Writes at 10 complete · watermark remains at 9", ["mass", "velocities"], [9,9],
     [[(10,"3.0")],[(10,"v₃")]],
     "A current read (end = UINT64_MAX) fails: their changes are beyond the watermark."),
    ("2. Watermark advances to 10 →", ["mass", "velocities"], [10,10],
     [[(10,"3.0")],[(10,"v₃")]],
     "The same changes are now red. Read these values before later updates.")])
save("watermarks", "Different watermarks for different attributes", [
    ("Producers advance independently", ["mass", "velocities"], [12,10],
     [[(8,"2.0"),(10,"3.0"),(12,"4.0")],[(8,"v₁"),(10,"v₂"),(12,"v₃")]],
     "At 12, mass is within its watermark; velocities is beyond its watermark.")])


# The read example keeps the same attribute and ordinal axis in every panel.
# An outlined, crossed-out rectangle records an old change without implying
# that its payload remains available to read.
out = []
top = 12
steps = [
    ("1. Write mass = 2.0 at ordinal 10", 9, [(10, "2.0")],
     "Cannot read: only change is at 10, beyond W.\nNo change exists at or below watermark 9.", "Cannot read: change at 10 is beyond W"),
    ("2. Advance the watermark to 10", 10, [(10, "2.0")],
     "Returns 2.0", "Returns 2.0"),
    ("3. Write mass = 3.0 at ordinal 11", 10, [(10, "2.0"), (11, "3.0")],
     "Cannot read: change at 11 is beyond W", "Out of range: 2.0 is no longer available to read"),
    ("4. Advance the watermark to 11", 11, [(10, "2.0"), (11, "3.0")],
     "Returns 3.0", "Still out of range: 2.0 is not restored"),
]
for index, (title, watermark, changes, latest, fixed) in enumerate(steps):
    items, end = panel(top, title, ["mass"], [watermark], [changes], "")
    out.extend(items)
    if index >= 2:
        x = LEFT + STEP*2 + 9
        y = top + 81
        out += [rect(x, y, STEP-18, 30, "#ffffff", 3),
                f'<rect x="{x}" y="{y}" width="{STEP-18}" height="30" rx="3" fill="none" stroke="{RED}" stroke-dasharray="4 3"/>',
                text(x+(STEP-18)/2, y+21, "2.0", 12, RED, "middle"),
                f'<path d="M{x+8},{y+25} L{x+STEP-26},{y+5}" stroke="{RED}" stroke-width="1.5"/>']
    y = end-30
    for x, label, result in [(24, "Current read (end = UINT64_MAX)", latest), (410, "Range read [10, 10]", fixed)]:
        out += [rect(x,y,366,82,"#f1f4f6",5),
                text(x+12,y+22,label,14,weight="bold"),
                *[text(x+12,y+46+18*i,line,13) for i,line in enumerate(result.split("\n"))]]
    top=y+107
out += [text(24,top+8,"Filled rectangle: current value. Crossed outline: earlier value unavailable to read.",13),
        text(24,top+32,"Red: at or below watermark. Green: beyond watermark. W = watermark.",13)]
height=top+54
(ROOT / "read-ranges.svg").write_text(f'''<svg xmlns="http://www.w3.org/2000/svg" width="800" height="{height}" viewBox="0 0 800 {height}" role="img" aria-labelledby="title">
<title id="title">Current (end UINT64_MAX) and fixed-range reads of mass on /World/BodyA as writes and watermarks advance</title>
<!-- SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &amp; AFFILIATES. All rights reserved. -->
<!-- SPDX-License-Identifier: LicenseRef-NvidiaProprietary -->
<rect width="800" height="{height}" fill="white"/>
<g font-family="Arial, Helvetica, sans-serif">{''.join(out)}</g></svg>''', encoding="utf-8")



def save_diagram(name, title, height, items):
    """Write the introductory diagrams with the timelines' colors and typography."""
    (ROOT / (name + ".svg")).write_text(f'''<svg xmlns="http://www.w3.org/2000/svg" width="800" height="{height}" viewBox="0 0 800 {height}" role="img" aria-labelledby="title">
<title id="title">{escape(title)}</title>
<!-- SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &amp; AFFILIATES. All rights reserved. -->
<!-- SPDX-License-Identifier: LicenseRef-NvidiaProprietary -->
<rect width="800" height="{height}" fill="white"/>
<defs><marker id="arrow" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse"><path d="M0,0 L10,5 L0,10" fill="#536575"/></marker></defs>
<g font-family="Arial, Helvetica, sans-serif">{''.join(items)}</g></svg>''', encoding="utf-8")


# Keep composition and population distinct, then show the runtime update cycle.
out = []
for x, y, w, title, detail in [
    (24, 24, 180, "USD layers on disk", "Scene and assets"),
    (310, 24, 180, "USD stage", "Composed scene"),
    (596, 24, 180, "Runtime stage", "Values + changes"),
    (566, 190, 210, "1. Write changes", "At a new ordinal"),
    (292, 190, 216, "2. Advance watermark", "Wait for completion"),
    (24, 190, 210, "3. Read values", "Consume, then release results"),
]:
    out += [rect(x, y, w, 78, "#f1f4f6", 6),
            text(x+w/2, y+29, title, 16, anchor="middle", weight="bold"),
            text(x+w/2, y+55, detail, 12, anchor="middle")]
for path in ["M204,63 H304", "M490,63 H590", "M686,102 V184",
             "M566,230 H514", "M292,230 H240", "M129,268 V314 H671 V274"]:
    out.append(f'<path d="{path}" fill="none" stroke="#536575" stroke-width="2" marker-end="url(#arrow)"/>')
out += [text(257, 49, "Load + compose", 12, anchor="middle"),
        text(543, 49, "Population", 13, anchor="middle", weight="bold"),
        text(400, 304, "Repeat runtime updates", 13, anchor="middle"),
        text(24, 355, "Population writes at a supplied ordinal; advance its watermark before reading.", 13)]
save_diagram("population-flow", "USD composition, population, and runtime updates", 380, out)

# Group the same USDA values by attribute, with prim labels inside each column.
out = [text(24, 30, "Runtime values populated at ordinal 1", 18, weight="bold"),
       text(24, 57, "Watermark = 1 · both prims use the same attribute types", 13),
       text(344, 93, "/World/BodyA", 14, anchor="middle", weight="bold"),
       text(616, 93, "/World/BodyB", 14, anchor="middle", weight="bold")]
for y, name, dtype, values in [
    (110, "mass", "float", ["2.0", "3.0"]),
    (192, "velocities", "vector3f[]", ["[(1, 0, 0)]", "[(0, 1, 0)]"]),
]:
    out += [text(24, y+28, name, 16, weight="bold"), text(24, y+50, dtype, 13),
            rect(200, y, 552, 64, "#fbe9e8", 4)]
    for x, value in zip([216, 488], values):
        out += [rect(x, y+10, 256, 44, RED, 4),
                text(x+128, y+38, value, 16, "white", "middle", "bold")]
out += [text(24, 293, "Each rectangle is one prim’s attribute value, written at ordinal 1.", 13),
        text(24, 317, "Only mass and velocities are shown; hierarchy and other metadata are omitted.", 13)]
save_diagram("runtime-layout", "USDA values grouped into runtime attribute columns", 342, out)
