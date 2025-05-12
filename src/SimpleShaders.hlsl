
cbuffer ObjectConstants : register(b0)
{
    float4x4 model;
    float4x4 mvp;
};

cbuffer LightConstants : register(b2)
{
    float4 ambientColor;
    float4 lightColor;
    float3 lightPosition; // World space
    float3 cameraPosition; // World space
};



cbuffer MaterialConstants : register(b3) {
    float4 specularColor;
    float specularPower;
};


Texture2D g_texture : register(t0);

// Static sampler bound to sampler register s0 (defined in Root Signature)
SamplerState g_sampler : register(s0);


// Simple vertex structure matching our input layour
struct VertexInput
{
    float3 position : POSITION;
    float4 color : COLOR;
    float2 texcoord : TEXCOORD;
    float3 normal : NORMAL;
};

// Data passed from Vertex Shader to Pixel Shader
struct VertexOutput
{
    float4 position : SV_POSITION;
    float4 color : COLOR;
    float2 texcoord : TEXCOORD;
    float3 normal : NORMAL;
    float3 worldNormal : WORLD_NORMAL;
    float3 worldPos : WORLD_POSITION;
};

VertexOutput VSMain(VertexInput input) {
    VertexOutput output;

    output.position = mul(mvp, float4(input.position, 1.0f));
    output.color = input.color;
    output.texcoord = input.texcoord;
    output.normal = input.normal;
    output.worldNormal = mul((float3x3)model, input.normal);
    output.worldPos = mul(model, float4(input.position, 1.0f)).xyz;
    return output;
}

float4 PSMain(VertexOutput input) : SV_TARGET {
    float3 normal = normalize(input.worldNormal);
    float3 lightDir = normalize(lightPosition - input.worldPos);
    float3 viewDir = normalize(cameraPosition - input.worldPos);

    float diffFactor = max(dot(normal, lightDir), 0.0f);
    float4 diffuse = diffFactor * lightColor;
    
    float3 halfwayDir = normalize(lightDir + viewDir);
    float specFactor = pow(max(dot(normal, halfwayDir), 0.0f), specularPower);
    float4 specular = specFactor * specularColor;

    float4 lighting = ambientColor + diffuse + specular;

    float4 textureColor = g_texture.Sample(g_sampler, input.texcoord);
    
    float4 finalColor = saturate(textureColor * lighting);
        
    return finalColor;
}