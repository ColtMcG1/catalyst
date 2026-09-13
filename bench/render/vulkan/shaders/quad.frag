// Fragment stage shared by every quad benchmark: writes the interpolated vertex colour, so the cost of a fragment is
// as close to zero as the hardware allows and the numbers reflect geometry and submission rather than shading.
// License: CDDL-1.0 (see LICENSE).
#version 450

layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 out_color;

void main()
{
    out_color = v_color;
}
