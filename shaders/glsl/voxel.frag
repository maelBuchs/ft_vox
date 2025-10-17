#version 450

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inUV;

layout(location = 0) out vec4 outFragColor;

void main() {
    vec3 lightDir = normalize(vec3(0.5, 1.0, 0.3));
    float diff = max(dot(inNormal, lightDir), 0.0);

    float ambient = 0.6;
    float diffuse = 0.4 * diff;
    float lighting = ambient + diffuse;

    outFragColor = vec4(inColor.rgb * lighting, inColor.a);
}
