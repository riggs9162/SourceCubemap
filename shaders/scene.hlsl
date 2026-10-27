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
};
struct Constants {
    column_major float4x4 projection;
    float4 tint;
    float4 controls;
    uint4 flags;
    float4 fog;
};
[[vk::push_constant]] Constants parameters;
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
    output.position = mul(parameters.projection, float4(input.position, 1));
    output.uv = input.uv;
    output.lightUV = input.lightUV;
    output.color = input.color;
    output.blend = input.blend;
    output.fogPosition = float3(output.position.xy, output.position.w);
    return output;
}

float4 PSMain(FragmentInput input) : SV_Target0 {
    uint flags = parameters.flags.x;
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
    if ((flags & 1) != 0) { light = 1; }
    if ((flags & 64) != 0) {
        float3 detail = detailTexture.Sample(materialSampler, input.uv * parameters.controls.z).rgb;
        base.rgb *= lerp(1, detail * 2, parameters.controls.y);
    }
    float3 result = base.rgb * light;
    if ((flags & 32) != 0) { result = lerp(result, base.rgb, base.a); }
    if ((flags & 128) == 0) { result *= input.color.rgb; }
    result *= parameters.tint.rgb;
    if (parameters.flags.w != 0) {
        float fogStart = asfloat(parameters.flags.z);
        float factor = saturate((length(input.fogPosition) - fogStart) / (parameters.controls.w - fogStart));
        result = lerp(result, parameters.fog.rgb, factor);
    }
    return float4(result, base.a * parameters.tint.a);
}
