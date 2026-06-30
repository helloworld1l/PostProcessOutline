#version 430 core

uniform sampler2D maskTexture;
uniform sampler2D sceneDepthTexture;
uniform sampler2D selectedDepthTexture;
uniform vec3 outlineColor;
uniform vec4 outlineScreenParams;
uniform float outlineThicknessPixels;
uniform int outlineOcclusionAware;
uniform int outlineMode;

in vec2 texCoord;
out vec4 fragColor;

const int kMaxKernelRadius = 8;
const float kDepthEpsilon = 1.0e-4;
const int kModeDilateOnly = 0;
const int kModeSobelOnly = 1;
const int kModeDilateSobelHybrid = 2;
const int kModeBlenderStyle = 3;

float sampleVisibleMask(vec2 uv)
{
    vec2 clampedUv = clamp(uv, vec2(0.0), vec2(1.0));
    if (texture(maskTexture, clampedUv).r < 0.5) {
        return 0.0;
    }
    if (outlineOcclusionAware == 0) {
        return 1.0;
    }

    float selectedDepth = texture(selectedDepthTexture, clampedUv).r;
    float sceneDepth = texture(sceneDepthTexture, clampedUv).r;
    return selectedDepth <= (sceneDepth + kDepthEpsilon) ? 1.0 : 0.0;
}

float computeDilateWeight(vec2 uv, int radius, vec2 texelStep)
{
    float bestWeight = 0.0;
    for (int y = -kMaxKernelRadius; y <= kMaxKernelRadius; ++y) {
        if (abs(y) > radius) {
            continue;
        }
        for (int x = -kMaxKernelRadius; x <= kMaxKernelRadius; ++x) {
            if (abs(x) > radius) {
                continue;
            }

            vec2 sampleUv = uv + (vec2(float(x), float(y)) * texelStep);
            if (sampleUv.x < 0.0 || sampleUv.x > 1.0 || sampleUv.y < 0.0 || sampleUv.y > 1.0) {
                continue;
            }
            if (sampleVisibleMask(sampleUv) < 0.5) {
                continue;
            }

            float distancePixels = length(vec2(float(x), float(y)));
            float normalizedDistance = radius > 0 ? clamp(distancePixels / float(radius), 0.0, 1.0) : 1.0;
            bestWeight = max(bestWeight, 1.0 - normalizedDistance);
        }
    }
    return bestWeight;
}

float computeSobelWeight(vec2 uv, vec2 texelStep)
{
    float tl = sampleVisibleMask(uv + vec2(-texelStep.x,  texelStep.y));
    float tc = sampleVisibleMask(uv + vec2( 0.0,          texelStep.y));
    float tr = sampleVisibleMask(uv + vec2( texelStep.x,  texelStep.y));
    float ml = sampleVisibleMask(uv + vec2(-texelStep.x,  0.0));
    float mr = sampleVisibleMask(uv + vec2( texelStep.x,  0.0));
    float bl = sampleVisibleMask(uv + vec2(-texelStep.x, -texelStep.y));
    float bc = sampleVisibleMask(uv + vec2( 0.0,         -texelStep.y));
    float br = sampleVisibleMask(uv + vec2( texelStep.x, -texelStep.y));

    float gx = -tl - 2.0 * ml - bl + tr + 2.0 * mr + br;
    float gy =  tl + 2.0 * tc + tr - bl - 2.0 * bc - br;
    float magnitude = length(vec2(gx, gy));
    return clamp(magnitude * 0.25, 0.0, 1.0);
}

// Blender-style outline: pure 8-sample Sobel with hard threshold.
// No dilation loop — constant cost (8 texture samples) regardless of thickness setting.
// Gives a precise 1-pixel mask boundary line, fully opaque, no gradient.
float computeBlenderStyleWeight(vec2 uv, vec2 texelStep)
{
    float tl = sampleVisibleMask(uv + vec2(-texelStep.x,  texelStep.y));
    float tc = sampleVisibleMask(uv + vec2( 0.0,          texelStep.y));
    float tr = sampleVisibleMask(uv + vec2( texelStep.x,  texelStep.y));
    float ml = sampleVisibleMask(uv + vec2(-texelStep.x,  0.0));
    float mr = sampleVisibleMask(uv + vec2( texelStep.x,  0.0));
    float bl = sampleVisibleMask(uv + vec2(-texelStep.x, -texelStep.y));
    float bc = sampleVisibleMask(uv + vec2( 0.0,         -texelStep.y));
    float br = sampleVisibleMask(uv + vec2( texelStep.x, -texelStep.y));
    float gx = -tl - 2.0 * ml - bl + tr + 2.0 * mr + br;
    float gy =  tl + 2.0 * tc + tr - bl - 2.0 * bc - br;
    return length(vec2(gx, gy)) > 0.1 ? 1.0 : 0.0;
}

void main()
{
    if (sampleVisibleMask(texCoord) > 0.5) {
        fragColor = vec4(0.0);
        return;
    }

    int radius = clamp(int(ceil(outlineThicknessPixels)), 1, kMaxKernelRadius);
    vec2 texelStep = outlineScreenParams.zw;

    float outlineAlpha = 0.0;
    if (outlineMode == kModeDilateOnly) {
        outlineAlpha = computeDilateWeight(texCoord, radius, texelStep);
    } else if (outlineMode == kModeSobelOnly) {
        outlineAlpha = computeSobelWeight(texCoord, texelStep);
    } else if (outlineMode == kModeBlenderStyle) {
        outlineAlpha = computeBlenderStyleWeight(texCoord, texelStep);
    } else {
        float sobelWeight = computeSobelWeight(texCoord, texelStep);
        float dilateWeight = computeDilateWeight(texCoord, radius, texelStep);
        outlineAlpha = max(dilateWeight, sobelWeight);
    }

    if (outlineAlpha <= 0.001) {
        fragColor = vec4(0.0);
        return;
    }

    fragColor = vec4(outlineColor, outlineAlpha);
}

