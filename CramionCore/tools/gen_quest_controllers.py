# Genera CramionCore/src/asset/QuestControllers.inc: la malla de los mandos de
# Meta Quest 3 (Touch Plus) para el modelo integrado del XR Controller.
#
#   python -I gen_quest_controllers.py left.glb right.glb salida.inc
#
# Los .glb son los de @webxr-input-profiles/assets 1.0 (MIT, Amazon),
# dist/profiles/meta-quest-touch-plus/{left,right}.glb. Estan en el espacio
# grip de WebXR (el mismo que la pose Grip de OpenXR), en metros: cada nodo
# con malla se hornea con su transform del mundo (botones en reposo). Sin
# texturas: el motor les pone un material oscuro.

import json
import math
import struct
import sys


def load_glb(path):
    data = open(path, 'rb').read()
    magic, _, length = struct.unpack_from('<III', data, 0)
    if magic != 0x46546C67:
        raise SystemExit(path + ': no es un .glb')
    offset = 12
    doc = None
    blob = None
    while offset < length:
        size, kind = struct.unpack_from('<II', data, offset)
        offset += 8
        chunk = data[offset:offset + size]
        offset += size
        if kind == 0x4E4F534A:
            doc = json.loads(chunk)
        elif kind == 0x004E4942:
            blob = chunk
    return doc, blob


COMPONENTS = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4}
FORMATS = {5120: 'b', 5121: 'B', 5122: 'h', 5123: 'H', 5125: 'I', 5126: 'f'}


def read_accessor(doc, blob, index):
    acc = doc['accessors'][index]
    view = doc['bufferViews'][acc['bufferView']]
    n = COMPONENTS[acc['type']]
    fmt = FORMATS[acc['componentType']]
    size = struct.calcsize(fmt)
    stride = view.get('byteStride', n * size)
    base = view.get('byteOffset', 0) + acc.get('byteOffset', 0)
    return [struct.unpack_from('<' + fmt * n, blob, base + i * stride) for i in range(acc['count'])]


def mat_mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def trs(node):
    if 'matrix' in node:
        m = node['matrix']  # columnas
        return [[m[c * 4 + r] for c in range(4)] for r in range(4)]
    tx, ty, tz = node.get('translation', [0, 0, 0])
    x, y, z, w = node.get('rotation', [0, 0, 0, 1])
    sx, sy, sz = node.get('scale', [1, 1, 1])
    r = [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
         [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
         [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]
    return [[r[0][0] * sx, r[0][1] * sy, r[0][2] * sz, tx],
            [r[1][0] * sx, r[1][1] * sy, r[1][2] * sz, ty],
            [r[2][0] * sx, r[2][1] * sy, r[2][2] * sz, tz],
            [0, 0, 0, 1]]


def bake(path):
    doc, blob = load_glb(path)
    positions, normals, indices = [], [], []

    def visit(index, parent):
        node = doc['nodes'][index]
        world = mat_mul(parent, trs(node))
        if 'mesh' in node:
            for prim in doc['meshes'][node['mesh']]['primitives']:
                if prim.get('mode', 4) != 4:
                    continue
                base = len(positions)
                for p in read_accessor(doc, blob, prim['attributes']['POSITION']):
                    positions.append(tuple(sum(world[r][c] * (p[c] if c < 3 else 1.0) for c in range(4)) for r in range(3)))
                for nx, ny, nz in read_accessor(doc, blob, prim['attributes']['NORMAL']):
                    # Escala uniforme: basta con girar y normalizar.
                    v = [world[r][0] * nx + world[r][1] * ny + world[r][2] * nz for r in range(3)]
                    length = math.sqrt(sum(c * c for c in v)) or 1.0
                    normals.append(tuple(c / length for c in v))
                for (i,) in read_accessor(doc, blob, prim['indices']):
                    indices.append(base + i)
        for child in node.get('children', []):
            visit(child, world)

    identity = [[1 if r == c else 0 for c in range(4)] for r in range(4)]
    for root in doc['scenes'][doc.get('scene', 0)]['nodes']:
        visit(root, identity)
    if len(positions) > 65535:
        raise SystemExit(path + ': demasiados vertices para indices de 16 bits')
    return positions, normals, indices


def emit(out, name, mesh):
    positions, normals, indices = mesh

    def rows(values, per_line):
        return '\n'.join('    ' + ', '.join(values[i:i + per_line]) + ',' for i in range(0, len(values), per_line))

    # Decimas de milimetro en int16 (el mando mide ~16 cm) y normales en int8.
    out.write('// %s: %d vertices, %d triangulos.\n' % (name, len(positions), len(indices) // 3))
    out.write('constexpr std::int16_t k%sPositions[] = {\n' % name)
    out.write(rows([str(int(round(c * 10000.0))) for p in positions for c in p], 24) + '\n};\n')
    out.write('constexpr std::int8_t k%sNormals[] = {\n' % name)
    out.write(rows([str(max(-127, min(127, int(round(c * 127.0))))) for n in normals for c in n], 30) + '\n};\n')
    out.write('constexpr std::uint16_t k%sIndices[] = {\n' % name)
    out.write(rows([str(i) for i in indices], 24) + '\n};\n\n')


def main():
    if len(sys.argv) != 4:
        raise SystemExit('uso: gen_quest_controllers.py left.glb right.glb salida.inc')
    left, right = bake(sys.argv[1]), bake(sys.argv[2])
    with open(sys.argv[3], 'w', encoding='utf-8', newline='\n') as out:
        out.write('// Generado por CramionCore/tools/gen_quest_controllers.py: no editar a mano.\n')
        out.write('// Mandos de Meta Quest 3 (Touch Plus) de @webxr-input-profiles/assets 1.0\n')
        out.write('// (MIT, Copyright (c) 2019 Amazon; ver THIRD_PARTY_NOTICES.md), en el espacio\n')
        out.write('// grip: posiciones en decimas de milimetro, normales en int8 (x127).\n\n')
        emit(out, 'QuestLeft', left)
        emit(out, 'QuestRight', right)


main()
