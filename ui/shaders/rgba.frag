#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;
layout(binding = 0) uniform sampler2D tex;
void main()
{
    // 解码侧已经用 VideoProcessor 转成 RGBA，这里只做采样输出
    fragColor = texture(tex, v_uv);
}