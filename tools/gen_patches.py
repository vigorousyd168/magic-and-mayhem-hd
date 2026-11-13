"""Generate src/ddraw/patches.h from a capstone listing of Chaos.exe.

Every screen-size expression in the game is an inlined ternary on IsHighRes:
    neg r; sbb r,r; and r,MASK; add r,BASE      (MASK 0xA0 = 800-640, 0x78 = 600-480)
With IsHighRes forced on, the high-res value can be moved to any size by editing
one immediate per site, without changing code length.
"""
import re, sys

rx = re.compile(r'^([0-9a-f]{8})  (\S+) ?(.*?)(?:    ; (.*))?$')
ins = []
notes = []
for l in open(sys.argv[1], encoding='utf-8'):
    m = rx.match(l.rstrip('\n'))
    if m:
        ins.append((int(m[1], 16), m[2], m[3]))
        notes.append(m[4] or '')
idx = {a: i for i, (a, _, _) in enumerate(ins)}

def names_a_resource(i):
    """Size values that are formatted into a file name or config key must stay 800x600."""
    return any(ins[j][1] == 'push' and '%' in notes[j] for j in range(i + 1, min(len(ins), i + 22)))

def size(i):
    return ins[i + 1][0] - ins[i][0]

def imm_addr(i):
    """Address of the 32-bit immediate at the end of instruction i."""
    return ins[i][0] + size(i) - 4

# Left alone: a fixed dialog offset whose height add is too short to patch,
# and the realm map hit test, which divides the mouse by the screen size to get
# 800x600 silhouette coordinates while the map itself stays 800x600,
# the realm map's right-aligned icons, which must stay inside that 800x600 view,
# and the character screen's half-screen portrait surface, whose extra area is
# black and would be pasted over the background.
PANEL_DW = {0x4ccf29, 0x53b731, 0x539fe5, 0x4ab603}
PANEL_DH = {0x4ccf47, 0x53b744, 0x4ab633}

W_MASK, H_MASK = 0xa0, 0x78
out = []   # (addr, old, kind)
counts = {}
def add(addr, old, kind):
    out.append((addr, old, kind)); counts[kind] = counts.get(kind, 0) + 1

for i, (a, mn, ops) in enumerate(ins):
    m = re.fullmatch(r'(e[a-z]{2}), 0x([0-9a-f]+)', ops) if mn == 'and' else None
    if not m or int(m[2], 16) not in (W_MASK, H_MASK):
        continue
    reg, mask = m[1], int(m[2], 16)
    if not any(ins[j][1] == 'sbb' and ins[j][2] == f'{reg}, {reg}' for j in range(max(0, i - 8), i)):
        continue
    if names_a_resource(i):
        counts['skipped_name'] = counts.get('skipped_name', 0) + 1
        continue
    if mask == W_MASK:
        if a in PANEL_DW:
            continue
        add(imm_addr(i), W_MASK, 'K_ADD_DW')
    else:
        if a in PANEL_DH:
            continue
        for j in range(i + 1, min(len(ins), i + 9)):
            q = re.fullmatch(reg + r', 0x([0-9a-f]+)', ins[j][2])
            if ins[j][1] == 'add' and q:
                if size(j) < 5:
                    sys.exit(f'short add at {ins[j][0]:x}')
                add(imm_addr(j), int(q[1], 16), 'K_ADD_DH')
                break
            q = re.fullmatch(r'e[a-z]{2}, \[' + reg + r' \+ 0x([0-9a-f]+)\]', ins[j][2])
            if ins[j][1] == 'lea' and q and size(j) >= 6:
                add(imm_addr(j), int(q[1], 16), 'K_ADD_DH')
                break
            if ins[j][2].startswith(reg):
                print(f'; skipped dH site {a:x}: {ins[j][1]} {ins[j][2]}', file=sys.stderr)
                break

# "x = W - (W==640 ? 320 : W==800 ? 400 : 0)" centring code in the terrain renderer
for i, (a, mn, ops) in enumerate(ins):
    if mn == 'cmp' and re.fullmatch(r'e[a-z]{2}, 0x320', ops) and 0x4b8000 <= a < 0x4bb000:
        add(imm_addr(i), 0x320, 'K_SET_W')
        for j in range(i + 1, i + 10):
            if ins[j][1] == 'mov' and ins[j][2].endswith(', 0x190') and size(j) >= 5:
                add(imm_addr(j), 0x190, 'K_SET_W2')
                break
        else:
            sys.exit(f'unexpected shape at {a:x}')

# battlefield clip rectangles
for a, old, kind in ((0x55a424, 0x320, 'K_SET_W'), (0x55a42f, 0x258, 'K_SET_H'), (0x55a43c, 0x1cc, 'K_SET_H_140'),
                     (0x55b6d3, 0x320, 'K_SET_W'), (0x55b6db, 0x258, 'K_SET_H'), (0x55b6e5, 0x1cc, 'K_SET_H_140')):
    add(imm_addr(idx[a]), old, kind)

# battle speech boxes: the speaker's screen position is worked out from a hard-coded screen centre
for a, old, kind in ((0x55a540, 0x190, 'K_ADD_DW2'), (0x55a54a, 0x12c, 'K_ADD_DH2'),
                     (0x55b13e, 0x190, 'K_ADD_DW2'), (0x55b14e, 0xe6, 'K_ADD_DH2'), (0x55b157, 0x12c, 'K_ADD_DH2'),
                     (0x55b4ea, 0x190, 'K_ADD_DW2'), (0x55b505, 0xe6, 'K_ADD_DH2'), (0x55b51a, 0x12c, 'K_ADD_DH2'),
                     (0x55b791, 0x190, 'K_ADD_DW2'), (0x55b7b1, 0xe6, 'K_ADD_DH2'), (0x55b7d3, 0x12c, 'K_ADD_DH2')):
    add(imm_addr(idx[a]), old, kind)

# viewport rectangle table in .data: {0,0,800,600} and {0,0,800,460}
for addr, old, kind in ((0x5be5a0, 800, 'K_SET_W'), (0x5be5a4, 600, 'K_SET_H'), (0x5be5b0, 800, 'K_SET_W'), (0x5be5b4, 460, 'K_SET_H_140')):
    add(addr, old, kind)

# occlusion bitmap: 339 rows x 344 bits, indexed by (2y+x)/8 and (2y-x+W)/8, so it grows with the screen
for a, old, kind in ((0x4f1154, 0x38f1, 'K_OCC_BYTES'), (0x4f1159, 0x153, 'K_OCC_ROWS'), (0x4f1163, 0x158, 'K_OCC_BITS'),
                     (0x4f116d, 0x2b, 'K_OCC_PITCH'), (0x4f1177, 0x1c788, 'K_OCC_TOTAL'), (0x4f1181, 0x38f1, 'K_OCC_BYTES')):
    add(imm_addr(idx[a]), old, kind)

# bug in the original game: when a wizard deployment site is occupied, the loop in Map.cpp
# jumps past the "next site" step, so every later site in the list is ignored and battles
# without enough places of power can fail with "No place to put wizard".
# The jump target moves back 3 bytes to the "add ebp,0x28" it should have run.
add(imm_addr(idx[0x4e1636]), 0x1a2, 'K_SUB_3')

with open(sys.argv[2], 'w', newline='\n') as f:
    f.write('/* generated by tools/gen_patches.py - do not edit */\n')
    f.write('enum { K_ADD_DW, K_ADD_DH, K_ADD_DW2, K_ADD_DH2, K_SET_W, K_SET_W2, K_SET_H, K_SET_H_140, K_OCC_ROWS, K_OCC_BITS, K_OCC_PITCH, K_OCC_TOTAL, K_OCC_BYTES, K_SUB_3 };\n')
    f.write('static const struct { DWORD addr, old; int kind; } patches[] = {\n')
    for a, old, kind in sorted(out):
        f.write(f'    {{0x{a:08x}, 0x{old:x}, {kind}}},\n')
    f.write('};\n')
print(len(out), counts)
