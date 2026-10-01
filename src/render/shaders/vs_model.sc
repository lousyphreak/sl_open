$input a_position, a_color0, a_texcoord0, a_texcoord1
$output v_color0, v_texcoord0, v_texcoord1, v_modelClipDistance

#include <bgfx_shader.sh>

uniform vec4 u_uvRect;
uniform vec4 u_tint;
uniform vec4 u_materialDiffuse;
uniform vec4 u_modelClipPlane;

void main()
{
	// Retail applies its nonlinear sqrt depth only after clipping polygons
	// in camera space. Full-mesh GPU submission must retain projective Z
	// through the fixed-function clipper; applying sqrt depth here moves the
	// near-plane intersection and removes pieces of triangles that cross it.
	// Reverse infinite Z preserves the same near-to-far ordering while
	// allowing hardware to clip the complete mesh at the authored plane.
	gl_Position = mul(u_modelViewProj, vec4(a_position, 1.0));
	v_modelClipDistance = dot(u_modelClipPlane, vec4(a_position, 1.0));
	v_color0 =
		mix(vec4_splat(1.0), a_color0, u_materialDiffuse.x)
			* u_tint;
	v_texcoord0 = u_uvRect.xy + a_texcoord0 * u_uvRect.zw;
	v_texcoord1 = a_texcoord1;
}
