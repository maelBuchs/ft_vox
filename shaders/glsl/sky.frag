#version 460

// Input from vertex shader
layout(location = 0) in vec3 inViewDir;

// Output color
layout(location = 0) out vec4 outFragColor;

// Push constants (same as vertex shader)
layout(push_constant) uniform constants {
    mat4 inverseViewProj;
    float timeOfDay; // 0.0 to 1.0
    float padding1;
    float padding2;
    float padding3;
}
PushConstants;

// Constants for sky rendering
const float PI = 3.14159265359;
const float TWO_PI = 6.28318530718;
const float AXIAL_TILT = 0.32; // radians ~18 degrees for subtle seasonal tilt

// Function to calculate sun/moon direction based on time of day
vec3 getCelestialDirection(float time) {
    // Align the cycle so that:
    // time = 0.25 -> sunrise, 0.5 -> noon, 0.75 -> sunset, 1.0 -> midnight.
    float angle = time * TWO_PI - 0.5 * PI;

    float cosAngle = cos(angle);
    float sinAngle = sin(angle);

    // Elliptical path with a small axial tilt so the sun/moon arc feels natural.
    vec3 dir = vec3(cosAngle, sinAngle, cosAngle * AXIAL_TILT);

    return normalize(dir);
}

// Function to calculate sky color based on sun position
vec3 getSkyColor(vec3 viewDir, vec3 sunDir, float time) {
    float sunHeight = sunDir.y; // -1 to 1

    // Interpolate between zenith (top of screen) and horizon colors
    float horizonMix = smoothstep(-0.25, 0.85, viewDir.y);

    const vec3 dayTopColor = vec3(0.16, 0.46, 0.94);
    const vec3 dayHorizonColor = vec3(0.68, 0.79, 0.95);
    const vec3 nightTopColor = vec3(0.00, 0.01, 0.05);
    const vec3 nightHorizonColor = vec3(0.05, 0.08, 0.15);
    const vec3 sunsetTopColor = vec3(0.54, 0.27, 0.52);
    const vec3 sunsetHorizonColor = vec3(1.05, 0.46, 0.12);

    vec3 dayColor = mix(dayHorizonColor, dayTopColor, horizonMix);
    vec3 nightColor = mix(nightHorizonColor, nightTopColor, horizonMix);
    vec3 duskColor = mix(sunsetHorizonColor, sunsetTopColor, horizonMix);

    float dayFactor = smoothstep(-0.08, 0.25, sunHeight);
    float nightFactor = 1.0 - smoothstep(-0.2, 0.05, sunHeight);
    float duskFactor = exp(-pow(sunHeight * 6.0, 2.0));

    vec3 sky = mix(nightColor, dayColor, dayFactor);
    sky = mix(sky, duskColor, duskFactor);

    // Boost zenith brightness during the day
    float zenithBoost = pow(clamp(viewDir.y, 0.0, 1.0), 3.0) * dayFactor;
    sky += vec3(0.02, 0.04, 0.07) * zenithBoost;

    // Subtle night glow near the zenith to avoid pure black
    float nightGlow = 1.0 - smoothstep(-0.35, -0.02, sunHeight);
    nightGlow *= nightGlow;
    sky += vec3(0.01, 0.012, 0.018) * nightGlow * (1.0 - horizonMix);

    return sky;
}

vec2 projectToDisc(vec3 direction, vec3 centerDir) {
    vec3 upReference = abs(centerDir.y) > 0.9 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    vec3 right = normalize(cross(upReference, centerDir));
    if (length(right) < 1e-4) {
        upReference = vec3(1.0, 0.0, 0.0);
        right = normalize(cross(upReference, centerDir));
    }
    vec3 up = normalize(cross(centerDir, right));
    return vec2(dot(direction, right), dot(direction, up));
}

void main() {
    // Normalize the view direction
    vec3 viewDir = normalize(inViewDir);

    // Calculate sun direction
    vec3 sunDir = getCelestialDirection(PushConstants.timeOfDay);

    // Moon is opposite to the sun
    vec3 moonDir = -sunDir;

    // Get base sky color
    vec3 skyColor = getSkyColor(viewDir, sunDir, PushConstants.timeOfDay);

    // --- Sun contribution -------------------------------------------------
    vec2 sunPlane = projectToDisc(viewDir, sunDir);
    float sunRadius = 0.048;
    float sunDistance = length(sunPlane);
    float sunCore = smoothstep(sunRadius, sunRadius - 0.004, sunDistance);
    float sunCorona = smoothstep(sunRadius + 0.02, sunRadius - 0.012, sunDistance);
    float sunVisibility = smoothstep(-0.35, 0.08, sunDir.y);
    float sunRim = clamp(1.0 - sunDistance / sunRadius, 0.0, 1.0);
    float sunStreak = sin(12.0 * atan(sunPlane.y, sunPlane.x)) * 0.06 * sunRim;
    vec3 sunBodyColor = mix(vec3(1.35, 1.08, 0.60), vec3(1.0, 0.78, 0.35),
                            clamp(sunDistance / sunRadius, 0.0, 1.0));
    sunBodyColor += vec3(0.08, 0.04, 0.0) * sunStreak;
    skyColor += sunBodyColor * sunVisibility * (sunCore * 1.6 + sunCorona * 0.45);

    const float moonRadius = 0.038;
    vec2 moonPlane = projectToDisc(viewDir, moonDir);
    float moonDistance = length(moonPlane);
    float moonSurface = smoothstep(moonRadius, moonRadius - 0.004, moonDistance);
    float moonHalo = smoothstep(moonRadius + 0.022, moonRadius - 0.01, moonDistance);
    float moonVisibility =
        smoothstep(-0.48, -0.05, moonDir.y) * (1.0 - smoothstep(-0.02, 0.18, sunDir.y));

    if (moonVisibility > 0.0) {
        vec2 normalizedCoords = moonPlane / max(moonRadius, 1e-4);
        float radial = length(normalizedCoords);

        // Lighting direction projected onto the moon plane for 2D phase control
        vec2 moonLightPlane = projectToDisc(normalize(-sunDir), moonDir);
        moonLightPlane =
            (length(moonLightPlane) > 1e-4) ? normalize(moonLightPlane) : vec2(1.0, 0.0);

        float phase = dot(normalizedCoords, moonLightPlane);
        float litAmount = smoothstep(-0.45, 0.35, phase);

        float rimShadow = smoothstep(0.7, 1.0, radial);
        float centerGlow = 1.0 - smoothstep(0.0, 0.9, radial);

        float craterNoiseA = sin(normalizedCoords.x * 16.0) * sin(normalizedCoords.y * 18.0);
        float craterNoiseB = sin(length(normalizedCoords) * 22.0);
        float craterMask = smoothstep(0.0, 0.85, 1.0 - radial);
        float craterPattern = (craterNoiseA * 0.06 + craterNoiseB * 0.04) * craterMask;

        vec3 moonTerminatorColor = vec3(0.32, 0.34, 0.4);
        vec3 moonLitColor = vec3(0.86, 0.88, 0.95);
        vec3 moonSurfaceColor = mix(moonTerminatorColor, moonLitColor, litAmount);
        moonSurfaceColor -= vec3(0.06, 0.07, 0.09) * craterPattern;

        moonSurfaceColor += vec3(0.04, 0.045, 0.06) * centerGlow * litAmount;
        moonSurfaceColor *= mix(1.0, 0.7, rimShadow);

        skyColor += moonSurfaceColor * moonSurface * moonVisibility;
        skyColor += vec3(0.54, 0.6, 0.72) * moonHalo * moonVisibility * 0.3;
    }

    skyColor = clamp(skyColor, 0.0, 1.0);

    outFragColor = vec4(skyColor, 1.0);
}
