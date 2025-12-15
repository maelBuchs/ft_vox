#version 450
layout(push_constant) uniform PushConstants {
    mat4 renderMatrix; // ViewProjection
    vec4 blockPos;     // .xyz = position monde du bloc, .w = padding
}
pc;

const vec3 cubeVertices[24] = vec3[](
    // -- Face du bas (y=0) --
    vec3(0, 0, 0), vec3(1, 0, 0), vec3(1, 0, 0), vec3(1, 0, 1), vec3(1, 0, 1), vec3(0, 0, 1),
    vec3(0, 0, 1), vec3(0, 0, 0),

    // -- Face du haut (y=1) --
    vec3(0, 1, 0), vec3(1, 1, 0), vec3(1, 1, 0), vec3(1, 1, 1), vec3(1, 1, 1), vec3(0, 1, 1),
    vec3(0, 1, 1), vec3(0, 1, 0),

    // -- Piliers verticaux --
    vec3(0, 0, 0), vec3(0, 1, 0), vec3(1, 0, 0), vec3(1, 1, 0), vec3(1, 0, 1), vec3(1, 1, 1),
    vec3(0, 0, 1), vec3(0, 1, 1));

void main() {
    // Récupérer la position locale du sommet actuel (0.0 à 1.0)
    vec3 localPos = cubeVertices[gl_VertexIndex];

    // Calculer la position monde : Position du bloc + Position locale
    vec3 worldPos = pc.blockPos.xyz + localPos;

    // Projection standard
    gl_Position = pc.renderMatrix * vec4(worldPos, 1.0);
}