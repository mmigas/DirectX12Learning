// Raytracing.hlsl (Corrected)

// Define payload structure
struct RayPayload {
    float4 color;
    float visibility;
    // Add other data like recursion depth later if needed
};

// --- Global Root Signature Bindings ---
RWTexture2D<float4> g_output : register(u0);
RaytracingAccelerationStructure g_tlas : register(t0);

cbuffer CameraParams : register(b1) {
    float4x4 invViewProjection;
    float3 cameraPosition;
};

Texture2D g_texture : register(t1);
SamplerState g_sampler : register(s0); // Ensure this is used with g_texture

struct Vertex {
    float3 position;
    float4 color; // If you have per-vertex color, ensure it's used or remove
    float2 texCoord;
    float3 normal;
};
StructuredBuffer<Vertex> g_vertexBuffer : register(t2);
ByteAddressBuffer g_indexBuffer : register(t3);

cbuffer DXRObjectConstants : register(b2) {
    float4x4 worldMatrix;
    float4x4 invTransposeWorldMatrix;
};

cbuffer LightConstants : register(b3) {
    float4 ambientColor;
    float4 lightColor;
    float3 lightPosition;
    // float3 _pad1; // Padding defined in C++, not needed here if offsets are correct
};

cbuffer MaterialConstants : register(b4) {
    float4 specularColor;
    float specularPower;
};


// --- Ray Generation Shader ---
[shader("raygeneration")]
void RayGen() {
    uint2 dispatchIndex = DispatchRaysIndex().xy;
    uint2 size = DispatchRaysDimensions().xy;

    float ndcX = ((float)dispatchIndex.x + 0.5f) / (float)size.x * 2.0f - 1.0f;
    float ndcY = ((float)dispatchIndex.y + 0.5f) / (float)size.y * -2.0f + 1.0f;

    float4 nearPointNDC = float4(ndcX, ndcY, 0.0f, 1.0f);
    float4 nearPointWorld = mul(invViewProjection, nearPointNDC);
    nearPointWorld /= nearPointWorld.w;

    float3 calculatedRayOrigin = cameraPosition;
    float3 calculatedRayDirection = normalize(nearPointWorld.xyz - calculatedRayOrigin);

    RayPayload payload;
    payload.color = float4(0.0f, 0.0f, 0.0f, 1.0f); // Initialize color
    payload.visibility = -999.0f; // Initialize visibility (will be set by CHS or Miss)

    RayDesc ray;
    ray.Origin = calculatedRayOrigin;
    ray.Direction = calculatedRayDirection;
    ray.TMin = 0.01f;
    ray.TMax = 10000.0f;

    TraceRay(
        g_tlas,
        RAY_FLAG_NONE,
        0xFF,
        0, // Ray Contribution To Hit Group Index (Primary hit group)
        1, // Multiplier for Geometry Index
        0, // Miss Shader Index (Primary miss shader)
        ray,
        payload
    );

    g_output[dispatchIndex] = payload.color;
}

// --- Miss Shader (Primary Rays) ---
[shader("miss")]
void Miss(inout RayPayload payload) {
    payload.color = float4(0.1f, 0.1f, 0.1f, 1.0f); // Dark blue/grey for primary miss
}

// --- Closest Hit Shader (Primary Rays) ---
// Exported as "ClosestHit" in C++, used by Primary HitGroup
[shader("closesthit")]
void ClosestHit(inout RayPayload payload, BuiltInTriangleIntersectionAttributes attribs) {
    // 1. Calculate actual hit point attributes (worldPosition, worldNormal)
    float3 bary = float3(1.0 - attribs.barycentrics.x - attribs.barycentrics.y, attribs.barycentrics.x, attribs.barycentrics.y);
    uint primitiveIdx = PrimitiveIndex();
    uint indexSizeInBytes = 4; // Assuming R32_UINT indices
    uint indexOffset = primitiveIdx * 3 * indexSizeInBytes;

    uint i0 = g_indexBuffer.Load(indexOffset);
    uint i1 = g_indexBuffer.Load(indexOffset + indexSizeInBytes);
    uint i2 = g_indexBuffer.Load(indexOffset + indexSizeInBytes * 2);

    Vertex v0 = g_vertexBuffer[i0];
    Vertex v1 = g_vertexBuffer[i1];
    Vertex v2 = g_vertexBuffer[i2];

    float3 objectNormal = v0.normal * bary.x + v1.normal * bary.y + v2.normal * bary.z;
    float3 objectPosition = v0.position * bary.x + v1.position * bary.y + v2.position * bary.z;
    float2 hitTexCoord = v0.texCoord * bary.x + v1.texCoord * bary.y + v2.texCoord * bary.z; // For texturing later

    float3 worldPosition = mul(worldMatrix, float4(objectPosition, 1.0)).xyz;
    float3 worldNormal = normalize(mul((float3x3)invTransposeWorldMatrix, objectNormal));

    // 2. --- Shadow Ray Tracing Section ---
    payload.color = float4(0.0f, 0.0f, 0.0f, 1.0f); // Fallback dark grey if neither shadow shader runs

    float shadowRayBias = 0.005f;
    RayDesc shadowRay;
    shadowRay.Origin = worldPosition + worldNormal * shadowRayBias; // Origin from actual hit point

    // Direction towards the actual light source
    float3 shadowDirVector = lightPosition - shadowRay.Origin;
    
    shadowRay.Direction = normalize(shadowDirVector); 
    
    shadowRay.TMin = 0.0f; // Start immediately from biased origin
    shadowRay.TMax = length(shadowDirVector); // Ray length is distance to light
                                              // If TMax is zero or negative, ray is invalid.
                                              // DXR might treat this as an immediate miss.

    TraceRay(
        g_tlas,
        RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
        RAY_FLAG_SKIP_CLOSEST_HIT_SHADER |
        RAY_FLAG_FORCE_NON_OPAQUE, // Standard shadow flags
        0xFF,       // Instance Mask
        1,          // Ray Contribution To Hit Group Index (ShadowHitGroup - SBT Index 1)
        1,          // Multiplier for Geometry Index
        1,          // Miss Shader Index (ShadowMiss - SBT Miss Index 1)
        shadowRay,
        payload     // payload.color and .visibility will be set by ShadowAnyHit or ShadowMiss
    );
    float3 lightDir = normalize(lightPosition - worldPosition);
    float3 viewDir = normalize(cameraPosition - worldPosition);

    float4 ambient = ambientColor;
    float4 diffuse = max(dot(worldNormal, lightDir), 0.0f) * lightColor;
    
    float3 halfwayDir = normalize(lightDir + viewDir);
    float specFactor = pow(max(dot(worldNormal, halfwayDir), 0.0f), specularPower);
    float4 specular = specFactor * specularColor;

    float4 lighting = ambient + (diffuse + specular) * payload.visibility;

    float4 textureColor = g_texture.SampleLevel(g_sampler, hitTexCoord, 0.0f);

    payload.color = saturate(textureColor * lighting);
}
// --- Shadow Miss Shader ---
[shader("miss")] // This must match the export name "ShadowMiss" from C++
void ShadowMiss(inout RayPayload payload) { // Function name must match C++ export: ShadowMiss
    payload.visibility = 1.0f; // Standard: 1.0 for lit

}

// --- Shadow Any Hit Shader ---
[shader("anyhit")]
void ShadowAnyHit(inout RayPayload payload, BuiltInTriangleIntersectionAttributes attribs) {
    payload.visibility = 0.0f; // Standard: 0.0 for shadowed
    AcceptHitAndEndSearch(); 
}