#version 450

layout(set = 0, binding = 1) uniform sampler2D scene_texture;
layout(set = 0, binding = 2) uniform sampler2D outgoing_texture;

// weights_* match blur.frag so the incoming side keeps the Pause Blur look.
// transition: x = eased progress, y = shape (0 fade, 1 wipe, 2 grow), z = edge feather.
// geometry: wipe maps uv to its travel distance as dot(uv, xy) + z;
//           grow measures length((uv - xy) * zw).
layout(push_constant) uniform PushConstants {
    layout(offset = 48) vec4 weights_a;
    vec4 weights_b;
    vec4 transition;
    vec4 geometry;
} push_constants;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

vec4 incoming() {
    vec4 value = textureLod(scene_texture, uv, 0.0) * push_constants.weights_a.x;
    value += textureLod(scene_texture, uv, 1.0) * push_constants.weights_a.y;
    value += textureLod(scene_texture, uv, 2.0) * push_constants.weights_a.z;
    value += textureLod(scene_texture, uv, 3.0) * push_constants.weights_a.w;
    value += textureLod(scene_texture, uv, 4.0) * push_constants.weights_b.x;
    value += textureLod(scene_texture, uv, 5.0) * push_constants.weights_b.y;
    return value;
}

float reveal() {
    float progress = push_constants.transition.x;
    int shape = int(push_constants.transition.y + 0.5);
    if (shape == 0) {
        return progress;
    }
    float travelled;
    if (shape == 1) {
        travelled = dot(uv, push_constants.geometry.xy) + push_constants.geometry.z;
    } else {
        travelled = length((uv - push_constants.geometry.xy) * push_constants.geometry.zw);
    }
    // The edge starts one feather before the surface and ends one feather past it,
    // so progress 0 and 1 are fully outgoing and fully incoming.
    float feather = push_constants.transition.z;
    float edge = progress * (1.0 + feather);
    return 1.0 - smoothstep(edge - feather, edge, travelled);
}

void main() {
    color = mix(textureLod(outgoing_texture, uv, 0.0), incoming(), reveal());
}
