$input v_color0, v_texcoord0, v_texcoord1, v_modelClipDistance

#include <bgfx_shader.sh>

SAMPLER2D(s_texture, 0);

void main()
{
	if (v_modelClipDistance < 0.0) discard;
	gl_FragColor = texture2D(s_texture, v_texcoord0) * v_color0;
}
