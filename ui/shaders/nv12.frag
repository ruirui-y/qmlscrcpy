#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;
layout(binding = 0) uniform sampler2D texY;
layout(binding = 1) uniform sampler2D texUV;
void main()
{
    // BT.601 有限范围，和 GL 版着色器保持同一套系数
    float y = texture(texY, v_uv).r - 0.0625;
    float u = texture(texUV, v_uv).r - 0.5;
    float v = texture(texUV, v_uv).g - 0.5;
    vec3 rgb;
    rgb.r = 1.164 * y + 1.596 * v;
    rgb.g = 1.164 * y - 0.391 * u - 0.813 * v;
    rgb.b = 1.164 * y + 2.018 * u;
    fragColor = vec4(rgb, 1.0);
}