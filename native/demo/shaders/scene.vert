// SPDX-License-Identifier: GPL-3.0-only
#version 450
layout(location = 0) in vec3 position;
layout(location = 1) in vec3 color;
layout(location = 0) out vec3 vertexColor;
void main() {
    // Vulkan's positive-height viewport has the opposite Y orientation to DXGI.
    gl_Position = vec4(position.x, -position.y, position.z, 1.0);
    vertexColor = color;
}
