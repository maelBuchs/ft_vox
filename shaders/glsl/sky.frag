#version 460

// Input from vertex shader
layout(location = 0) in vec3 inViewDir;

// Output color
layout(location = 0) out vec4 outFragColor;

// Blue noise texture for dithering
layout(set = 0, binding = 0) uniform sampler2D blueNoiseTexture;

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

// Atmospheric Scattering Parameters
const vec3 betaR = vec3(5.5e-6, 13.0e-6, 22.4e-6); // Rayleigh scattering coefficient
const vec3 betaM = vec3(21e-6);                    // Mie scattering coefficient
const float Hr = 7994.0;                           // Rayleigh scale height
const float Hm = 1200.0;                           // Mie scale height
const float earthRadius = 6360e3;
const float atmosphereRadius = 6420e3;
const float sunIntensity = 20.0;

// Scattering samples (reduce if performance is an issue)
const int numSamples = 8;      // Primary ray samples
const int numSamplesLight = 4; // Light ray samples

// Rayleigh phase function - determines how light scatters in air
float rayleighPhase(float cosTheta) {
    return (3.0 / (16.0 * PI)) * (1.0 + cosTheta * cosTheta);
}

// Henyey-Greenstein phase function for Mie scattering
float miePhase(float cosTheta, float g) {
    float g2 = g * g;
    return (1.0 / (4.0 * PI)) * ((1.0 - g2) / pow(1.0 + g2 - 2.0 * g * cosTheta, 1.5));
}

vec3 computeAtmosphericScattering(vec3 viewDir, vec3 sunDir, float groundAltitude) {
    // Camera position (slightly above ground)
    vec3 rayOrigin = vec3(0.0, earthRadius + groundAltitude, 0.0);

    // Check if ray hits atmosphere
    vec3 rayDir = normalize(viewDir);
    float a = dot(rayDir, rayDir);
    float b = 2.0 * dot(rayDir, rayOrigin);
    float c = dot(rayOrigin, rayOrigin) - (atmosphereRadius * atmosphereRadius);
    float discriminant = b * b - 4.0 * a * c;

    if (discriminant < 0.0) {
        return vec3(0.0); // Ray doesn't hit atmosphere
    }

    // Calculate intersection points
    float t0 = (-b - sqrt(discriminant)) / (2.0 * a);
    float t1 = (-b + sqrt(discriminant)) / (2.0 * a);
    t0 = max(t0, 0.0);

    float segmentLength = (t1 - t0) / float(numSamples);
    float cosTheta = dot(rayDir, sunDir);

    // Phase functions
    float phaseR = rayleighPhase(cosTheta);
    float phaseM = miePhase(cosTheta, 0.76); // g = 0.76 for forward scattering

    // Accumulated optical depth
    vec3 totalRayleigh = vec3(0.0);
    vec3 totalMie = vec3(0.0);
    float opticalDepthR = 0.0;
    float opticalDepthM = 0.0;

    // March through atmosphere
    for (int i = 0; i < numSamples; i++) {
        // Sample point along ray
        float t = t0 + segmentLength * (float(i) + 0.5);
        vec3 samplePos = rayOrigin + rayDir * t;
        float height = length(samplePos) - earthRadius;

        // Density at sample point
        float hr = exp(-height / Hr) * segmentLength;
        float hm = exp(-height / Hm) * segmentLength;

        opticalDepthR += hr;
        opticalDepthM += hm;

        // Sample light ray toward sun
        float opticalDepthLightR = 0.0;
        float opticalDepthLightM = 0.0;

        // Simplified light sampling (check if sun is visible from this point)
        for (int j = 0; j < numSamplesLight; j++) {
            float tLight = float(j) * segmentLength;
            vec3 lightSamplePos = samplePos + sunDir * tLight;
            float lightHeight = length(lightSamplePos) - earthRadius;

            if (lightHeight < 0.0)
                break; // Hit ground

            opticalDepthLightR += exp(-lightHeight / Hr) * segmentLength;
            opticalDepthLightM += exp(-lightHeight / Hm) * segmentLength;
        }

        // Calculate transmittance
        vec3 tau = betaR * (opticalDepthR + opticalDepthLightR) +
                   betaM * 1.1 * (opticalDepthM + opticalDepthLightM);
        vec3 attenuation = exp(-tau);

        totalRayleigh += attenuation * hr;
        totalMie += attenuation * hm;
    }

    // Final scattering
    vec3 scattering = sunIntensity * (totalRayleigh * betaR * phaseR + totalMie * betaM * phaseM);

    return scattering;
}

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
    vec3 viewDir = normalize(inViewDir);
    vec3 sunDir = getCelestialDirection(PushConstants.timeOfDay);
    vec3 moonDir = -sunDir;

    float sunFacing = dot(viewDir, sunDir);
    float sunFront = smoothstep(0.0, 0.02, sunFacing);

    float moonFacing = dot(viewDir, moonDir);
    float moonFront = smoothstep(0.0, 0.02, moonFacing);

    // Start with atmospheric scattering (this gives us sunset/sunrise colors!)
    vec3 skyColor = computeAtmosphericScattering(viewDir, sunDir, 1.0);

    // Add base sky gradient for areas scattering doesn't cover well
    vec3 baseSky = getSkyColor(viewDir, sunDir, PushConstants.timeOfDay);
    float scatterStrength = smoothstep(-0.1, 0.3, sunDir.y); // Fade scattering when sun is down
    skyColor = mix(baseSky * 0.3, skyColor, scatterStrength);

    // Only draw the sun if it’s in front of the camera
    if (sunFront > 0.0) {
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

        skyColor += sunBodyColor * sunVisibility * (sunCore * 1.6 + sunCorona * 0.45) * sunFront;
    }

    const float moonRadius = 0.038;
    float moonVisibility =
        smoothstep(-0.48, -0.05, moonDir.y) * (1.0 - smoothstep(-0.02, 0.18, sunDir.y));

    if (moonVisibility > 0.0 && moonFront > 0.0) {
        vec2 moonPlane = projectToDisc(viewDir, moonDir);
        float moonDistance = length(moonPlane);
        float moonSurface = smoothstep(moonRadius, moonRadius - 0.004, moonDistance);
        float moonHalo = smoothstep(moonRadius + 0.022, moonRadius - 0.01, moonDistance);

        if (moonSurface > 0.0) {
            vec2 normalizedCoords = moonPlane / max(moonRadius, 1e-4);
            float radial = length(normalizedCoords);

            // Moon is lit by the sun (not self-illuminated!)
            // Calculate sun direction relative to moon
            vec3 sunToMoon = normalize(-sunDir); // Sun lights the moon
            vec2 moonLightPlane = projectToDisc(sunToMoon, moonDir);
            moonLightPlane =
                (length(moonLightPlane) > 1e-4) ? normalize(moonLightPlane) : vec2(1.0, 0.0);

            // Moon phase based on sun lighting
            float phase = dot(normalizedCoords, moonLightPlane);
            float litAmount = smoothstep(-0.45, 0.35, phase);

            // Crater pattern
            float craterNoiseA = sin(normalizedCoords.x * 16.0) * sin(normalizedCoords.y * 18.0);
            float craterNoiseB = sin(length(normalizedCoords) * 22.0);
            float craterMask = smoothstep(0.0, 0.85, 1.0 - radial);
            float craterPattern = (craterNoiseA * 0.06 + craterNoiseB * 0.04) * craterMask;

            // Moon colors (grayscale, not sun-colored!)
            vec3 moonDarkColor = vec3(0.15, 0.15, 0.18); // Dark side
            vec3 moonLitColor = vec3(0.86, 0.88, 0.92);  // Lit side
            vec3 moonColor = mix(moonDarkColor, moonLitColor, litAmount);
            moonColor -= vec3(0.06, 0.07, 0.09) * craterPattern;

            skyColor += moonColor * moonSurface * moonVisibility * moonFront;
            skyColor += vec3(0.54, 0.6, 0.72) * moonHalo * moonVisibility * 0.2 * moonFront;
        }
    }

    // Final adjustments
    skyColor = clamp(skyColor, 0.0, 1.0);

    // Apply blue noise dithering
    vec2 noiseUV = gl_FragCoord.xy / 64.0; // Blue noise texture is 64x64
    float noise = texture(blueNoiseTexture, noiseUV).r;
    skyColor += (noise - 0.5) / 255.0; // ±1/255 code step, centered around zero
    skyColor = clamp(skyColor, 0.0, 1.0);

    // Optional: Add stars at night
    if (sunDir.y < -0.1) {
        float starStrength = smoothstep(-0.1, -0.3, sunDir.y);
        // Add procedural stars here if desired
    }

    vec3 encoded = skyColor * vec3(1.0 / 2.2);
    outFragColor = vec4(encoded, 1.0);
}
