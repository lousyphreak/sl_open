$input a_position, a_texcoord0, a_texcoord2, a_normal, a_tangent
$output v_color0, v_texcoord0, v_texcoord1, v_modelClipDistance

#include <bgfx_shader.sh>

uniform vec4 u_uvRect;
uniform vec4 u_tint;
uniform vec4 u_materialDiffuse;
uniform vec4 u_lightingEnvironmentU;
uniform vec4 u_lightingEnvironmentV;
uniform vec4 u_lightingBase;
uniform vec4 u_lightingParams;
uniform vec4 u_lightingPositionRadius[32];
uniform vec4 u_lightingDirectionType[32];
uniform vec4 u_lightingColorIntensity[32];
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

	float normal_blend = u_lightingParams.y;
	float primary_weight = 1.0 - normal_blend;
	vec3 secondary_normal = a_tangent.xyz;
	vec3 environment_normal =
		a_normal * primary_weight + secondary_normal * normal_blend;
	v_texcoord1 = vec2(
		dot(u_lightingEnvironmentU.xyz, environment_normal),
		dot(u_lightingEnvironmentV.xyz, environment_normal)) * 0.5 + 0.5;

	vec3 color = u_lightingBase.xyz;
	if (u_lightingBase.w > 0.5)
	{
		color += a_texcoord2;
	}
	for (int light_index = 0; light_index < 32; ++light_index)
	{
		if (float(light_index) >= u_lightingParams.z)
		{
			break;
		}
		vec4 position_radius = u_lightingPositionRadius[light_index];
		vec4 direction_type = u_lightingDirectionType[light_index];
		vec4 rgb_intensity = u_lightingColorIntensity[light_index];
		if (direction_type.w > 0.5)
		{
			float normal_dot =
				dot(
					direction_type.xyz
						* (primary_weight * rgb_intensity.w),
					a_normal)
				+ dot(
					direction_type.xyz
						* (normal_blend * rgb_intensity.w),
					secondary_normal);
			if (normal_dot > 0.0)
			{
				color += normal_dot * rgb_intensity.xyz;
			}
		}
		else
		{
			vec3 to_light =
				position_radius.xyz
					- u_lightingParams.x * a_position;
			float distance_squared = dot(to_light, to_light);
			float radius_squared =
				position_radius.w * position_radius.w;
			if (distance_squared < radius_squared)
			{
				float normal_dot = dot(to_light, a_normal);
				if (normal_blend > 0.0)
				{
					normal_dot = normal_dot * primary_weight
						+ dot(to_light, secondary_normal) * normal_blend;
				}
				if (normal_dot > 0.0)
				{
					float distance = sqrt(distance_squared);
					float attenuation = normal_dot
						* (1.0 / distance
							+ distance / radius_squared
							- 2.0 / position_radius.w);
					color += attenuation * rgb_intensity.w
						* rgb_intensity.xyz;
				}
			}
		}
	}
	if (u_lightingParams.w > 0.5)
	{
		color = min(color, vec3_splat(1.0));
	}
	// The CPU path stores the lit diffuse channel in normalized RGBA8.
	color = floor(clamp(color, 0.0, 1.0) * 255.0 + 0.5) / 255.0;
	v_color0 =
		mix(vec4_splat(1.0), vec4(color, 1.0), u_materialDiffuse.x)
			* u_tint;
	v_texcoord0 = u_uvRect.xy + a_texcoord0 * u_uvRect.zw;
}
