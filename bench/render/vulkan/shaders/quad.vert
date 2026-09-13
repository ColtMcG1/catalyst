// Vertex stage of the batched / per-draw quad benchmarks: passes a clip-space vertex through, transformed by the
// push-constant rect so one 6-vertex quad can be re-drawn at N different places without touching a buffer.
// License: CDDL-1.0 (see LICENSE).
#version 450

layout(location = 0) in vec2 in_position;
layout(location = 1) in vec4 in_color;

layout(push_constant) uniform quad_transform
{
    // xy = translation in clip space, zw = scale. (0, 0, 1, 1) leaves the vertex untouched.
    vec4 offset_scale;
} pc;

layout(location = 0) out vec4 v_color;

void main()
{
    gl_Position = vec4(in_position * pc.offset_scale.zw + pc.offset_scale.xy, 0.0, 1.0);
    v_color = in_color;
}
