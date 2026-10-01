$input v_color0, v_texcoord0, v_texcoord1, v_modelClipDistance

#include <bgfx_shader.sh>

SAMPLER2D(s_texture, 0);
SAMPLER2D(s_materialTexture, 1);

void main()
{
	if (v_modelClipDistance < 0.0) discard;
	vec4 primary = texture2D(s_texture, v_texcoord0);
	vec4 modifier = texture2D(s_materialTexture, v_texcoord1);
	gl_FragColor = (primary + modifier) * v_color0;
}
