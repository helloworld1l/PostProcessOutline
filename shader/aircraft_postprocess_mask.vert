#version 430 core

uniform mat4 mvpMat;

layout(location = 0) in vec3 vertexPosition;

void main()
{
    gl_Position = mvpMat * vec4(vertexPosition, 1.0);
}
