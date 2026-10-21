import math
import time

import pico3d

HERE = "/lib/plugins/teapot"
SWITCH_MS = 8000

BACKDROP_TOP = color.rgb(10, 12, 40)
BACKDROP_BOTTOM = color.rgb(2, 2, 8)


def load_mesh(path):
    with open(path, "rb") as f:
        head = f.read(8)
        if head[:4] != b"P3DM":
            raise ValueError("not a pico3d mesh: " + path)
        vertices = int.from_bytes(head[4:6], "little")
        triangles = int.from_bytes(head[6:8], "little")

        def section(size):
            buf = bytearray(size)
            if f.readinto(buf) != size:
                raise ValueError(path + " is truncated")
            return buf

        return pico3d.mesh(positions=section(12 * vertices),
                           normals=section(12 * vertices),
                           uvs=section(8 * vertices),
                           indices=section(6 * triangles))


MODELS = [
    (load_mesh(HERE + "/teapot.p3dm"),
     pico3d.material(color=color.rgb(230, 150, 70), shading=pico3d.material.GOURAUD,
                     specular=color.rgb(255, 240, 220), shininess=24),
     0.95),
    (load_mesh(HERE + "/suzanne.p3dm"),
     pico3d.material(color=color.rgb(120, 190, 255), shading=pico3d.material.GOURAUD,
                     specular=color.rgb(255, 255, 255), shininess=32),
     1.05),
]

sun = pico3d.light(direction=pico3d.vec3(-0.5, -0.6, -1.0),
                   color=color.rgb(255, 246, 232),
                   ambient=color.rgb(36, 32, 60))

view_projection = pico3d.mat4.perspective(45, 1.0, 0.1, 20).multiply(
    pico3d.mat4.look_at(pico3d.vec3(0, 0.4, 3.2), pico3d.vec3(0, 0, 0), pico3d.vec3(0, 1, 0)))

backdrop = brush.gradient(brush.LINEAR, 0, 0, 0, 128,
                          [(0.0, BACKDROP_TOP), (1.0, BACKDROP_BOTTOM)])

pico3d.engine.cores(2)

surfaces = {}
start = time.ticks_ms()


def surface_for(target):
    key = id(target)
    if key not in surfaces:
        surface = pico3d.surface(target)
        surfaces[key] = (target, surface, pico3d.scene(surface, meshes=1, vertices=1200, triangles=2300))
    return surfaces[key][1:]


def update():
    elapsed = time.ticks_diff(time.ticks_ms(), start)
    mesh, material, scale = MODELS[(elapsed // SWITCH_MS) % len(MODELS)]
    spin = elapsed * 0.06

    screen.pen = backdrop
    screen.rectangle(0, 0, screen.width, screen.height)

    surface, scene = surface_for(screen)
    model = (pico3d.mat4()
             .rotate_y(spin)
             .rotate_x(12 * math.sin(elapsed * 0.0011))
             .scale(scale))
    scene.reset()
    scene.add(mesh, model, view_projection, material, sun)
    surface.clear_depth()
    surface.draw(scene)
