$input v_color0, v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_texture, 0);
SAMPLER2D(s_palette, 1);

void main()
{
	vec2 indexed = texture2D(s_texture, v_texcoord0).rg;
	vec4 color = texture2D(s_palette, vec2(indexed.r * (255.0 / 256.0) + (0.5 / 256.0), 0.5));
	gl_FragColor = vec4(color.rgb, color.a * indexed.g) * v_color0;
}
