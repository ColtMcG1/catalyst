// Vertex stage of the rendering_basics example: passes a position and colour straight through.
// Matches the `vertex` struct in main.cpp (vec3 position at offset 0, vec3 colour at offset 12) via `vertex_layout`.
#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_color;

layout(location = 0) out vec3 v_color;

void main()
{
    gl_Position = vec4(in_position, 1.0);
    v_color = in_color;
}
