import math
import re
import struct
import sys
from collections import defaultdict, deque

SEGMENTS = 6
WELD_DIGITS = 5


def parse_three_teapot(path):
    source = open(path).read()
    source = re.sub(r"/\*.*?\*/", "", source, flags=re.S)

    def array(name):
        body = re.search(r"const " + name + r" = \[(.*?)\];", source, re.S).group(1)
        return [float(value) for value in re.findall(r"-?\s*\d+(?:\.\d+)?", body.replace(" ", ""))]

    patches = [int(value) for value in array("teapotPatches")]
    vertices = array("teapotVertices")
    control_points = [tuple(vertices[i:i + 3]) for i in range(0, len(vertices), 3)]
    patch_list = [patches[i:i + 16] for i in range(0, len(patches), 16)]
    return patch_list, control_points


def bernstein(t):
    s = 1.0 - t
    return (s * s * s, 3 * t * s * s, 3 * t * t * s, t * t * t)


def evaluate_patch(points, u, v):
    bu = bernstein(u)
    bv = bernstein(v)
    x = y = z = 0.0
    for row in range(4):
        for column in range(4):
            weight = bu[row] * bv[column]
            px, py, pz = points[row * 4 + column]
            x += weight * px
            y += weight * py
            z += weight * pz
    return (x, y, z)


def tessellate_teapot(patch_list, control_points, segments):
    positions = []
    triangles = []
    for patch in patch_list:
        points = [control_points[index] for index in patch]
        base = len(positions)
        for i in range(segments + 1):
            for j in range(segments + 1):
                positions.append(evaluate_patch(points, i / segments, j / segments))
        row = segments + 1
        for i in range(segments):
            for j in range(segments):
                a = base + i * row + j
                b = a + row
                triangles.append((a, b, a + 1))
                triangles.append((a + 1, b, b + 1))
    return positions, triangles


def z_up_to_y_up(positions):
    return [(x, z, -y) for x, y, z in positions]


def weld(positions, triangles):
    remap = {}
    welded = []
    index_of = []
    for position in positions:
        key = tuple(round(c, WELD_DIGITS) for c in position)
        if key not in remap:
            remap[key] = len(welded)
            welded.append(position)
        index_of.append(remap[key])
    kept = []
    for a, b, c in triangles:
        a, b, c = index_of[a], index_of[b], index_of[c]
        if a == b or b == c or a == c:
            continue
        if triangle_area(welded, (a, b, c)) < 1e-10:
            continue
        kept.append((a, b, c))
    return welded, kept


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def face_normal(positions, triangle):
    a, b, c = (positions[i] for i in triangle)
    return cross(sub(b, a), sub(c, a))


def triangle_area(positions, triangle):
    n = face_normal(positions, triangle)
    return 0.5 * math.sqrt(dot(n, n))


def orient_consistently(positions, triangles):
    edge_owners = defaultdict(list)
    for t, (a, b, c) in enumerate(triangles):
        for u, v in ((a, b), (b, c), (c, a)):
            edge_owners[(min(u, v), max(u, v))].append(t)

    def directed(triangle):
        a, b, c = triangle
        return {(a, b), (b, c), (c, a)}

    oriented = list(triangles)
    visited = [False] * len(triangles)
    components = []
    for seed in range(len(triangles)):
        if visited[seed]:
            continue
        visited[seed] = True
        component = [seed]
        queue = deque([seed])
        while queue:
            t = queue.popleft()
            edges = directed(oriented[t])
            for u, v in edges:
                for other in edge_owners[(min(u, v), max(u, v))]:
                    if visited[other]:
                        continue
                    if (u, v) in directed(oriented[other]):
                        a, b, c = oriented[other]
                        oriented[other] = (a, c, b)
                    visited[other] = True
                    component.append(other)
                    queue.append(other)
        components.append(component)

    for component in components:
        volume = 0.0
        for t in component:
            a, b, c = (positions[i] for i in oriented[t])
            volume += dot(a, cross(b, c))
        if volume < 0:
            for t in component:
                a, b, c = oriented[t]
                oriented[t] = (a, c, b)
    return oriented, len(components)


def smooth_normals(positions, triangles):
    sums = [[0.0, 0.0, 0.0] for _ in positions]
    for triangle in triangles:
        n = face_normal(positions, triangle)
        for index in triangle:
            sums[index][0] += n[0]
            sums[index][1] += n[1]
            sums[index][2] += n[2]
    normals = []
    for x, y, z in sums:
        length = math.sqrt(x * x + y * y + z * z) or 1.0
        normals.append((x / length, y / length, z / length))
    return normals


def fit_unit(positions):
    lo = [min(p[i] for p in positions) for i in range(3)]
    hi = [max(p[i] for p in positions) for i in range(3)]
    centre = [(lo[i] + hi[i]) / 2 for i in range(3)]
    radius = max(math.sqrt(sum((p[i] - centre[i]) ** 2 for i in range(3))) for p in positions)
    return [tuple((p[i] - centre[i]) / radius for i in range(3)) for p in positions]


def write_p3dm(path, positions, normals, triangles):
    with open(path, "wb") as f:
        f.write(b"P3DM" + struct.pack("<HH", len(positions), len(triangles)))
        for p in positions:
            f.write(struct.pack("<3f", *p))
        for n in normals:
            f.write(struct.pack("<3f", *n))
        f.write(bytes(8 * len(positions)))
        for t in triangles:
            f.write(struct.pack("<3H", *t))


def build(positions, triangles, path):
    positions, triangles = weld(positions, triangles)
    triangles, components = orient_consistently(positions, triangles)
    positions = fit_unit(positions)
    normals = smooth_normals(positions, triangles)
    write_p3dm(path, positions, normals, triangles)
    print(f"{path}: {len(positions)} vertices, {len(triangles)} triangles, {components} components")


def read_obj(path):
    positions = []
    triangles = []
    for line in open(path):
        parts = line.split()
        if not parts:
            continue
        if parts[0] == "v":
            positions.append(tuple(float(c) for c in parts[1:4]))
        elif parts[0] == "f":
            face = [int(p.split("/")[0]) - 1 for p in parts[1:]]
            for k in range(1, len(face) - 1):
                triangles.append((face[0], face[k], face[k + 1]))
    return positions, triangles


if __name__ == "__main__":
    kind, source, output = sys.argv[1:4]
    if kind == "teapot":
        patch_list, control_points = parse_three_teapot(source)
        positions, triangles = tessellate_teapot(patch_list, control_points, SEGMENTS)
        build(z_up_to_y_up(positions), triangles, output)
    elif kind == "obj":
        positions, triangles = read_obj(source)
        build(z_up_to_y_up(positions), triangles, output)
