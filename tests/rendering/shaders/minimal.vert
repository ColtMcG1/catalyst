// Minimal vertex stage used by the rendering tests: consumes no vertex inputs and emits a fixed position.
#version 450

void main()
{
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
}
