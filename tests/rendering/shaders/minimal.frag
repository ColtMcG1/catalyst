// Minimal fragment stage used by the rendering tests: writes opaque white to colour attachment 0.
#version 450

layout(location = 0) out vec4 out_color;

void main()
{
    out_color = vec4(1.0);
}
