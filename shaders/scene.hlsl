struct VertexInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float4 tangent : TANGENT;
    [[vk::location(3)]] float2 uv : TEXCOORD0;
    [[vk::location(4)]] float2 lightUV : TEXCOORD1;
    [[vk::location(5)]] float4 color : COLOR0;
    [[vk::location(6)]] float blend : TEXCOORD2;
    [[vk::location(7)]] float4 secondaryUV : TEXCOORD3;
};
struct FragmentInput {
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv : TEXCOORD0;
    [[vk::location(1)]] float2 lightUV : TEXCOORD1;
    [[vk::location(2)]] float4 color : COLOR0;
    [[vk::location(3)]] float blend : TEXCOORD2;
    [[vk::location(4)]] float3 fogPosition : TEXCOORD3;
    [[vk::location(5)]] float3 worldPosition : TEXCOORD4;
    [[vk::location(6)]] float3 worldNormal : TEXCOORD5;
    [[vk::location(7)]] float4 worldTangent : TEXCOORD6;
    [[vk::location(8)]] float4 secondaryUV : TEXCOORD7;
};
struct Camera {
    column_major float4x4 projection;
    float4 eye;
    float4 clipPlane;
    float4 waterView;
};
struct PropLight {
    float4 positionType;
    float4 color;
    float4 attenuation;
    float4 directionCone;
};
struct DrawUniform {
    float4 tint;
    float4 controls;
    uint4 flags;
    float4 fog;
    float4 skyTop, skyBottom, skySun, skySunColor, skyDusk, skyControls, stars;
    float4 phong, phongFresnel, phongTint;
    float4 ambient[6];
    PropLight lights[4];
    float4 waterFog, waterControls, waterReflect, waterRefract, waterScroll, waterLayers, waterTime;
    float4 selfIllumTint;
};
[[vk::push_constant]] Camera camera;
[[vk::binding(10)]] ConstantBuffer<DrawUniform> parameters;
[[vk::binding(0)]] Texture2D<float4> baseTexture;
[[vk::binding(1)]] Texture2D<float4> secondTexture;
[[vk::binding(2)]] Texture2D<float4> normalTexture;
[[vk::binding(3)]] Texture2D<float4> lightTexture;
[[vk::binding(4)]] Texture2D<float4> lightTexture1;
[[vk::binding(5)]] Texture2D<float4> lightTexture2;
[[vk::binding(6)]] Texture2D<float4> lightTexture3;
[[vk::binding(7)]] Texture2D<float4> detailTexture;
[[vk::binding(8)]] SamplerState materialSampler;
[[vk::binding(9)]] SamplerState lightSampler;
[[vk::binding(11)]] TextureCube<float4> waterReflection;
[[vk::binding(12)]] TextureCube<float4> waterRefraction;
[[vk::binding(13)]] Texture2D<float4> blendMaskTexture;
[[vk::binding(14)]] Texture2D<float4> selfIllumMaskTexture;
[[vk::binding(15)]] Texture2D<float4> phongExponentTexture;

FragmentInput VSMain(VertexInput input) {
    FragmentInput output;
    output.position = mul(camera.projection, float4(input.position, 1));
    output.uv = input.uv;
    output.lightUV = input.lightUV;
    output.color = input.color;
    output.blend = input.blend;
    output.fogPosition = float3(output.position.xy, output.position.w);
    output.worldPosition = input.position;
    output.worldNormal = input.normal;
    output.worldTangent = input.tangent;
    output.secondaryUV = input.secondaryUV;
    if (input.tangent.w == 2) {
        float3 toEye = camera.eye.xyz - input.position;
        float distance = max(length(toEye), 1);
        float scale = 1;
        if (input.tangent.z == 2) {
            scale = distance / 200;
            output.color.rgb *= clamp(19000 / (distance * distance), 0.05, 1);
        }
        if (input.tangent.z != 0) {
            float4 forward = mul(camera.projection, float4(input.position + toEye / distance * min(length(input.tangent.xy) * scale, distance * 0.5), 1));
            output.position.z = forward.z / forward.w * output.position.w;
        }
        output.position.xy += input.tangent.xy * scale * float2(1, -1);
    }
    return output;
}

float3 PaintedSkyColor(float3 direction) {
    direction = normalize(direction);
    float elevation = saturate(direction.z);
    float blend = pow(elevation, max(parameters.skyTop.w, 0.0001));
    float3 color = lerp(parameters.skyBottom.rgb, parameters.skyTop.rgb, blend) * parameters.skyBottom.w;
    float sunCosine = saturate(dot(direction, normalize(parameters.skySun.xyz)));
    if (parameters.skySun.w > 0) { color += parameters.skySunColor.rgb * pow(sunCosine, 20 / parameters.skySun.w); }
    if (parameters.skyDusk.w > 0) {
        float horizon = pow(max(1 - elevation, 0.000001), max(0.0, (2 - parameters.skyControls.x) * 4.4));
        float angle = pow(0.75 + 0.25 * sunCosine, 2.2);
        color += parameters.skyDusk.rgb * horizon * angle * parameters.skyDusk.w * parameters.skyBottom.w;
    }
    if (parameters.skyControls.y > 0 && elevation > 0) {
        float2 uv = direction.xy / (max(direction.z, 0.001) * max(parameters.stars.x, 0.0001)) + parameters.stars.z;
        float4 cloud = secondTexture.Sample(materialSampler, uv);
        color += cloud.rgb * pow(elevation, rcp(max(parameters.stars.y, 0.0001)));
    }
    return color;
}

float3 AmbientLighting(float3 normal) {
    float3 weights = normal * normal;
    return parameters.ambient[normal.x < 0 ? 1 : 0].rgb * weights.x
        + parameters.ambient[normal.y < 0 ? 3 : 2].rgb * weights.y
        + parameters.ambient[normal.z < 0 ? 5 : 4].rgb * weights.z;
}

float3 LightSample(PropLight light, float3 position, out float3 direction) {
    if (light.positionType.w == 1) {
        direction = normalize(light.positionType.xyz);
        return light.color.rgb;
    }
    float3 offset = light.positionType.xyz - position;
    float distance = max(length(offset), 0.001);
    direction = offset / distance;
    float attenuation = rcp(max(dot(light.attenuation.xyz, float3(1, distance, distance * distance)), 0.00001));
    if (light.positionType.w == 2) {
        float cone = dot(-direction, light.directionCone.xyz);
        attenuation *= pow(saturate((cone - light.directionCone.w) / max(light.color.w - light.directionCone.w, 0.00001)), light.attenuation.w);
    }
    return light.color.rgb * attenuation;
}

float3 WaterCoordinate(float3 direction) {
    float3 a = abs(direction);
    if (a.x >= a.y && a.x >= a.z) { return float3(float2(direction.x >= 0 ? -direction.z : direction.z, -direction.y) / a.x * 0.5 + 0.5, direction.x >= 0 ? 0 : 1); }
    if (a.y >= a.z) { return float3(float2(direction.x, direction.y >= 0 ? direction.z : -direction.z) / a.y * 0.5 + 0.5, direction.y >= 0 ? 2 : 3); }
    return float3(float2(direction.z >= 0 ? direction.x : -direction.x, -direction.y) / a.z * 0.5 + 0.5, direction.z >= 0 ? 4 : 5);
}

float3 WaterDirection(float3 coordinate) {
    float2 p = coordinate.xy * 2 - 1;
    uint face = uint(coordinate.z);
    if (face == 0) { return float3(1, -p.y, -p.x); }
    if (face == 1) { return float3(-1, -p.y, p.x); }
    if (face == 2) { return float3(p.x, 1, p.y); }
    if (face == 3) { return float3(p.x, -1, -p.y); }
    if (face == 4) { return float3(p.x, -p.y, 1); }
    return float3(-p.x, -p.y, -1);
}

float4 WaterColor(FragmentInput input) {
    bool above = parameters.waterControls.w != 0;
    clip(above ? camera.eye.z - camera.waterView.x : camera.waterView.x - camera.eye.z);
    float2 uv = input.uv * parameters.waterScroll.zw;
    float4 normal = normalTexture.Sample(materialSampler, uv + parameters.waterScroll.xy);
    if (any(parameters.waterLayers != 0)) {
        float4 offsets = parameters.waterLayers * parameters.waterTime.z;
        float2 rotated = float2(input.uv.x + input.uv.y, input.uv.y - input.uv.x) * 0.1;
        normal += normalTexture.Sample(materialSampler, rotated + offsets.xy);
        normal += normalTexture.Sample(materialSampler, input.uv.yx * 0.45 + offsets.zw);
        normal *= 0.33;
    }
    normal.xyz = normal.xyz * 2 - 1;
    float3 ray = input.worldPosition - camera.eye.xyz;
    float surfaceDistance = length(ray);
    float3 view = ray / max(surfaceDistance, 0.0001);
    float3 coordinates = WaterCoordinate(view);
    float4 unwarped = waterRefraction.SampleLevel(lightSampler, view, 0);
    float fogRange = parameters.waterControls.z - parameters.waterFog.w;
    float fogDepth = above ? saturate((max(unwarped.a - surfaceDistance, 0) - parameters.waterFog.w) / fogRange) : 1;
    float2 distortion = normal.xy * normal.a * fogDepth;
    float3 refractCoordinates = coordinates;
    refractCoordinates.xy += distortion * parameters.waterControls.y;
    float4 refracted = waterRefraction.SampleLevel(lightSampler, WaterDirection(refractCoordinates), 0);
    float fog = above ? saturate((max(refracted.a - surfaceDistance, 0) - parameters.waterFog.w) / fogRange - 0.05) : saturate((surfaceDistance - parameters.waterFog.w) / fogRange);
    float3 refraction = lerp(refracted.rgb * parameters.waterRefract.rgb, parameters.waterFog.rgb, fog);
    if (parameters.waterRefract.w == 0) { return float4(refraction, 1); }
    float3 reflectCoordinates = coordinates;
    reflectCoordinates.xy += distortion * parameters.waterControls.x;
    float3 reflectRay = WaterDirection(reflectCoordinates); reflectRay.z *= -1;
    float3 reflection = waterReflection.SampleLevel(lightSampler, reflectRay, 0).rgb * parameters.waterReflect.rgb;
    float3 tangent = normalize(input.worldTangent.xyz);
    float3 faceNormal = normalize(input.worldNormal);
    float3 bitangent = normalize(cross(faceNormal, tangent)) * input.worldTangent.w;
    float3 eyeTangent = float3(dot(-view, tangent), dot(-view, bitangent), dot(-view, faceNormal));
    float fresnel = pow(1 - saturate(dot(eyeTangent, normal.xyz)), 5) * saturate((fogDepth - 0.05) * 20);
    if (parameters.waterReflect.w != 0) { fresnel = 1; }
    return float4(lerp(refraction, reflection, fresnel), 1);
}

float4 PSMain(FragmentInput input) : SV_Target0 {
    uint flags = parameters.flags.x;
    clip(dot(float4(input.worldPosition, 1), camera.clipPlane));
    if ((flags & 2048) != 0) { return float4(PaintedSkyColor(input.worldPosition), camera.waterView.z == 2 ? 1000000 : 1); }
    if ((flags & 524288) != 0) { return WaterColor(input); }
    float4 base = baseTexture.Sample(materialSampler, input.uv);
    if ((flags & 4194304) != 0) { base *= secondTexture.Sample(materialSampler, input.secondaryUV.xy); }
    if ((flags & 512) != 0) { base.a *= input.color.a; }
    if ((flags & 2) != 0) {
        float blend = saturate(input.blend);
        if ((flags & 1048576) != 0) {
            float2 mask = blendMaskTexture.Sample(materialSampler, input.secondaryUV.zw).rg;
            float low = saturate(mask.y - mask.x), high = saturate(mask.y + mask.x);
            blend = high > low ? smoothstep(low, high, blend) : step(low, blend);
        }
        base = lerp(base, secondTexture.Sample(materialSampler, input.secondaryUV.xy), blend);
    }
    if ((flags & 4) != 0) { clip(base.a * parameters.tint.a - parameters.controls.x); }
    if ((flags & 8388608) != 0) {
        float3 factor = base.rgb;
        if ((flags & 16777216) != 0) { factor = pow(max(lerp(0.5, pow(max(base.rgb, 0), rcp(2.2)), base.a) * 2, 0), 2.2); }
        return float4(factor, 1);
    }
    float3 light = lightTexture.Sample(lightSampler, input.lightUV).rgb;
    if ((flags & 24) == 24) {
        float3 encoded = normalTexture.Sample(materialSampler, input.uv).xyz;
        float3 weights = encoded;
        if ((flags & 1024) == 0) {
            float3 normal = normalize(encoded * 2 - 1);
            weights = saturate(float3(dot(normal, float3(0.81649658,0,0.57735027)), dot(normal, float3(-0.40824829,0.70710678,0.57735027)), dot(normal, float3(-0.40824829,-0.70710678,0.57735027))));
            weights *= weights;
            weights /= max(dot(weights, 1), 0.00001);
        }
        light = weights.x * lightTexture1.Sample(lightSampler, input.lightUV).rgb + weights.y * lightTexture2.Sample(lightSampler, input.lightUV).rgb + weights.z * lightTexture3.Sample(lightSampler, input.lightUV).rgb;
    }
    if ((flags & 128) != 0) { light = input.color.rgb; }
    float3 normal = normalize(input.worldNormal);
    float4 normalSample = float4(0.5, 0.5, 1, 1);
    if ((flags & 8) != 0) {
        normalSample = normalTexture.Sample(materialSampler, input.uv);
        float3 tangent = normalize(input.worldTangent.xyz);
        float3 bitangent = normalize(cross(normal, tangent)) * input.worldTangent.w;
        float3 local = normalSample.xyz * 2 - 1;
        normal = normalize(tangent * local.x + bitangent * local.y + normal * local.z);
    }
    float3 specular = 0;
    if (parameters.flags.y != 0) {
        if (parameters.phong.w == 0) { light = AmbientLighting(normal); }
        float3 eye = normalize(camera.eye.xyz - input.worldPosition);
        float3 reflected = reflect(-eye, normal);
        float exponent = parameters.phong.x;
        float3 specularTint = parameters.phongTint.rgb;
        if ((flags & 131072) != 0) {
            float4 exponentSample = phongExponentTexture.Sample(materialSampler, input.uv);
            exponent = exponentSample.r * parameters.phongFresnel.w + 1;
            if ((flags & 65536) != 0) { specularTint *= lerp(1, base.rgb, exponentSample.g); }
        }
        for (uint i = 0; i < uint(parameters.phong.z); ++i) {
            float3 direction;
            float3 radiance = LightSample(parameters.lights[i], input.worldPosition, direction);
            float cosine = dot(normal, direction);
            float diffuse = saturate(cosine);
            if ((flags & 8192) != 0) { diffuse = pow(cosine * 0.5 + 0.5, 2); }
            if (parameters.phong.w == 0) { light += radiance * diffuse; }
            specular += radiance * pow(saturate(dot(reflected, direction)), exponent) * saturate(cosine);
        }
        float fresnel = pow(1 - saturate(dot(normal, eye)), 2);
        float3 ranges = parameters.phongFresnel.xyz;
        fresnel = fresnel < 0.5 ? lerp(ranges.x, ranges.y, fresnel * 2) : lerp(ranges.y, ranges.z, fresnel * 2 - 1);
        float mask = (flags & 16384) != 0 ? base.a : normalSample.a;
        if ((flags & 32768) != 0) { mask = 1 - mask; }
        specular *= specularTint * mask * fresnel * parameters.phong.y;
        if ((flags & 4096) == 0) { specular = 0; }
    }
    if ((flags & 1) != 0) { light = 1; }
    float3 detailEmission = 0;
    if ((flags & 64) != 0) {
        float4 detail = detailTexture.Sample(materialSampler, input.uv * parameters.controls.z);
        float detailFactor = parameters.controls.y;
        uint detailMode = uint(parameters.selfIllumTint.w);
        if (detailMode == 0) { base.rgb *= lerp(1, detail.rgb * 2, detailFactor); }
        else if (detailMode == 1) { base.rgb += detail.rgb * detailFactor; }
        else if (detailMode == 2) { base.rgb = lerp(base.rgb, detail.rgb, detailFactor * detail.a); }
        else if (detailMode == 3) { base.rgb = lerp(base.rgb, detail.rgb, detailFactor); }
        else if (detailMode == 4) { base.rgb = lerp(base.rgb, detail.rgb, detailFactor * (1 - base.a)); base.a = detail.a; }
        else if (detailMode == 5) { detailEmission = detail.rgb * detailFactor; }
        else if (detailMode == 8) { base.rgb *= lerp(1, detail.rgb, detailFactor); }
        else if (detailMode == 9) { base.a *= lerp(1, detail.a, detailFactor); }
        else if (detailMode == 11) { base.rgb *= dot(detail.rgb, 2.0 / 3.0); }
    }
    if ((flags & 262144) != 0) { base.rgb = lerp(base.rgb, lerp(base.rgb * parameters.tint.rgb, parameters.tint.rgb, parameters.phongTint.w), base.a); }
    float3 result = base.rgb * light + specular + detailEmission;
    if ((flags & 32) != 0) {
        float3 mask = (flags & 2097152) != 0 ? selfIllumMaskTexture.Sample(materialSampler, input.uv).rgb : base.aaa;
        result = lerp(base.rgb * light, base.rgb * parameters.selfIllumTint.rgb, mask) + specular;
    }
    if ((flags & 128) == 0) { result *= input.color.rgb; }
    if ((flags & 262144) == 0) { result *= parameters.tint.rgb; }
    if (parameters.flags.w != 0) {
        float fogStart = asfloat(parameters.flags.z);
        float factor = saturate((length(input.fogPosition) - fogStart) / (parameters.controls.w - fogStart));
        result = lerp(result, parameters.fog.rgb, factor);
    }
    if (camera.waterView.y != 0 && camera.waterView.z == 0 && camera.eye.w == 2) {
        float distance = length(input.worldPosition - camera.eye.xyz);
        float factor = saturate((distance - parameters.waterFog.w) / (parameters.waterControls.z - parameters.waterFog.w));
        result = lerp(result, parameters.waterFog.rgb, factor);
    }
    float alpha = base.a * parameters.tint.a;
    if (camera.waterView.z == 2) { alpha = camera.eye.w == 2 ? length(input.worldPosition - camera.eye.xyz) : 1000000; }
    return float4(result, alpha);
}
