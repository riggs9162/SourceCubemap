struct VertexInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float4 tangent : TANGENT;
    [[vk::location(3)]] float2 uv : TEXCOORD0;
    [[vk::location(4)]] float2 lightUV : TEXCOORD1;
    [[vk::location(5)]] float4 color : COLOR0;
    [[vk::location(6)]] float blend : TEXCOORD2;
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
};
struct Camera {
    column_major float4x4 projection;
    float4 eye;
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
    return output;
}

float3 PaintedSkyColor(float3 direction) {
    direction = normalize(direction);
    float elevation = saturate(direction.z);
    float blend = pow(elevation, max(parameters.skyTop.w, 0.0001));
    float3 color = lerp(parameters.skyBottom.rgb, parameters.skyTop.rgb, blend);
    if (parameters.skyControls.y > 0 && elevation > 0) {
        float2 uv = direction.xy / (max(direction.z, 0.001) * max(parameters.stars.x, 0.0001)) + parameters.stars.z;
        float4 cloud = secondTexture.Sample(materialSampler, uv);
        color += cloud.rgb * pow(elevation, rcp(max(parameters.stars.y, 0.0001)));
    }
    return color * parameters.skyBottom.w;
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

float4 PSMain(FragmentInput input) : SV_Target0 {
    uint flags = parameters.flags.x;
    if ((flags & 2048) != 0) { return float4(PaintedSkyColor(input.worldPosition), 1); }
    float4 base = baseTexture.Sample(materialSampler, input.uv);
    if ((flags & 512) != 0) { base.a *= input.color.a; }
    if ((flags & 2) != 0) { base = lerp(base, secondTexture.Sample(materialSampler, input.uv), saturate(input.blend)); }
    if ((flags & 4) != 0) { clip(base.a * parameters.tint.a - parameters.controls.x); }
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
            float4 exponentSample = secondTexture.Sample(materialSampler, input.uv);
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
    if ((flags & 64) != 0) {
        float3 detail = detailTexture.Sample(materialSampler, input.uv * parameters.controls.z).rgb;
        base.rgb *= lerp(1, detail * 2, parameters.controls.y);
    }
    if ((flags & 262144) != 0) { base.rgb = lerp(base.rgb, lerp(base.rgb * parameters.tint.rgb, parameters.tint.rgb, parameters.phongTint.w), base.a); }
    float3 result = base.rgb * light + specular;
    if ((flags & 32) != 0) { result = lerp(result, base.rgb, base.a); }
    if ((flags & 128) == 0) { result *= input.color.rgb; }
    if ((flags & 262144) == 0) { result *= parameters.tint.rgb; }
    if (parameters.flags.w != 0) {
        float fogStart = asfloat(parameters.flags.z);
        float factor = saturate((length(input.fogPosition) - fogStart) / (parameters.controls.w - fogStart));
        result = lerp(result, parameters.fog.rgb, factor);
    }
    return float4(result, base.a * parameters.tint.a);
}
