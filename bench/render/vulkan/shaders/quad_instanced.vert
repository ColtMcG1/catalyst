// Vertex stage of the instanced quad benchmark: a unit quad in binding 0 expanded by per-instance placement and colour
// in binding 1, so N quads cost one draw_indexed call.
// License: CDDL-1.0 (see LICENSE).
#version 450

layout(location = 0) in vec2 in_corner;    // Unit quad corner, [0, 1] x [0, 1].
layout(location = 1) in vec2 in_center;    // Per-instance centre in clip space.
layout(location = 2) in vec2 in_half_size; // Per-instance half extent in clip space.
layout(location = 3) in vec4 in_color;     // Per-instance colour.

layout(location = 0) out vec4 v_color;

void main()
{
    const vec2 corner = in_corner * 2.0 - 1.0;
    gl_Position = vec4(in_center + corner * in_half_size, 0.0, 1.0);
    v_color = in_color;
}
