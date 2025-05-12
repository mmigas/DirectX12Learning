// Raytracing.hlsl (Assignment 11 - Camera Rays & Texturing - Corrected)

// Define payload structure
struct RayPayload {
    float4 color;
    // Add other data like recursion depth later if needed
};

// --- Global Root Signature Bindings ---
// Param 0: Output UAV Table (u0)
RWTexture2D<float4> g_output : register(u0);
// Param 1: TLAS SRV (t0) - Root Descriptor
RaytracingAccelerationStructure g_tlas : register(t0);
// Param 2: Camera CBV Table (b1)
cbuffer CameraParams : register(b1) {
    float4x4 invViewProjection; // Corrected from invViewProjectionMatrix to match C++ DXRCameraConstants
    float3 cameraPosition;      // Corrected from cameraPositionW to match C++ DXRCameraConstants
    // float _pad1; // Padding is in C++ struct, not directly reflected here
};
// Param 3: Texture SRV Table (t1)
Texture2D g_texture : register(t1);
// Param 4: VB SRV Table (t2)
struct Vertex { // Ensure this matches the C++ Vertex struct exactly
    float3 position; // Changed from : POSITION semantic as it's raw data
    float4 color;
    float2 texCoord;
    float3 normal;
};
StructuredBuffer<Vertex> g_vertexBuffer : register(t2);
// Param 5: IB SRV Table (t3)
ByteAddressBuffer g_indexBuffer : register(t3);

// Sampler (Static Sampler)
SamplerState g_sampler : register(s0);

cbuffer DXRObjectConstants : register(b2) {               // Param 6: DXR Object CBV
    float4x4 worldMatrix;
    float4x4 invTransposeWorldMatrix; // For normals
};

cbuffer LightConstants : register(b3) {
    float4 ambientColor;
    float4 lightColor;       // Diffuse/Specular color of the light
    float3 lightPosition;    // World space
	float3 _pad1;
};
// Param 8: Material CBV Table (b4)
cbuffer MaterialConstants : register(b4) {
    float4 specularColor;    // Material's specular reflectivity color
    float specularPower;    // Material's shininess
};

// --- Ray Generation Shader ---
[shader("raygeneration")]
void RayGen() {
    uint2 dispatchIndex = DispatchRaysIndex().xy;
    uint2 size = DispatchRaysDimensions().xy;
    
    // Calculate normalized device coordinates (NDC) [-1, 1] for x, [1, -1] for y
    float ndcX = ((float)dispatchIndex.x + 0.5f) / (float)size.x * 2.0f - 1.0f;
    float ndcY = ((float)dispatchIndex.y + 0.5f) / (float)size.y * -2.0f + 1.0f; // Flip Y for typical DX convention
    
    // Unproject NDC coordinates to world space using inverse ViewProj matrix
    float4 nearPointNDC = float4(ndcX, ndcY, 0.0f, 1.0f); // Point on near plane in NDC
    float4 nearPointWorld = mul(invViewProjection, nearPointNDC);
    nearPointWorld /= nearPointWorld.w; // Perspective divide
    
    // Calculate ray origin and direction from camera
    float3 calculatedRayOrigin = cameraPosition; // Use cameraPosition from CB
    float3 calculatedRayDirection = normalize(nearPointWorld.xyz - calculatedRayOrigin);

    // Initialize payload BEFORE TraceRay
    RayPayload payload;
    payload.color = float4(0.0f, 0.0f, 0.0f, 1.0f); // Default to black (or another debug color)

    RayDesc ray;
    ray.Origin    = calculatedRayOrigin;    // Use camera-derived origin
    ray.Direction = calculatedRayDirection; // Use camera-derived direction
    ray.TMin      = 0.01f;                  // Start ray slightly in front of camera
    ray.TMax      = 10000.0f;               // Max distance

    // Trace the ray
    TraceRay(
        g_tlas,                     // Acceleration Structure
        RAY_FLAG_NONE,              // Ray Flags
        0xFF,                       // Instance Inclusion Mask (check all instances)
        0,                          // Ray Contribution To Hit Group Index (offset in SBT for this geometry type)
        1,                          // Multiplier for Geometry Index (stride in SBT if multiple geom types per hit group)
        0,                          // Miss Shader Index (offset in SBT for miss shaders)
        ray,                        // Ray description
        payload                     // Input/Output payload
    );

    // Write the final color from the payload (set by Miss or ClosestHit)
    g_output[dispatchIndex] =  payload.color;
}

// --- Miss Shader ---
// Executes when a ray doesn't hit anything in the TLAS
[shader("miss")]
void Miss(inout RayPayload payload) {
    // Set the payload color to a background color
    payload.color = float4(0.1f, 0.1f, 0.1f, 1.0f); // Blue background
}

// --- Closest Hit Shader ---
// Executes when a ray finds its closest hit
[shader("closesthit")]
void ClosestHit(inout RayPayload payload, BuiltInTriangleIntersectionAttributes attribs) {
    // Get hit info
    float3 barycentrics = float3(1.0f - attribs.barycentrics.x - attribs.barycentrics.y, attribs.barycentrics.x, attribs.barycentrics.y);
    uint primitiveIdx = PrimitiveIndex(); // Index of the triangle within the geometry

    // Base address in index buffer for this triangle's indices
    // Assuming 32-bit indices (4 bytes)
    uint indexSizeInBytes = 4;
    uint indexOffset = primitiveIdx * 3 * indexSizeInBytes;

    // Load the 3 vertex indices for this triangle
    uint i0 = g_indexBuffer.Load(indexOffset);
    uint i1 = g_indexBuffer.Load(indexOffset + indexSizeInBytes);
    uint i2 = g_indexBuffer.Load(indexOffset + indexSizeInBytes * 2);

    // Load the vertex data for the 3 vertices
    Vertex v0 = g_vertexBuffer[i0];
    Vertex v1 = g_vertexBuffer[i1];
    Vertex v2 = g_vertexBuffer[i2];

    // Interpolate attributes using barycentric coordinates
    float2 hitTexCoord = v0.texCoord * barycentrics.x + v1.texCoord * barycentrics.y + v2.texCoord * barycentrics.z;
    float3 objectNormal = v0.normal * barycentrics.x + v1.normal * barycentrics.y + v2.normal * barycentrics.z;
    float3 objectPosition = v0.position * barycentrics.x + v1.position * barycentrics.y + v2.position * barycentrics.z;

    float3 worldPosition = mul(worldMatrix, float4(objectPosition, 1.0)).xyz;
    float3 worldNormal = normalize(mul((float3x3)invTransposeWorldMatrix, objectNormal)); // Use invTranspose for normals
	
	float3 lightDir = normalize(lightPosition - worldPosition);
	float3 viewDir = normalize(cameraPosition - worldPosition);

	float4 ambient = ambientColor;

    float4 diffuse = max(dot(worldNormal, lightDir), 0.0f) * lightColor;
		
	float3 halfwayDir = normalize(lightDir + viewDir);
	float specFactor = pow(max(dot(worldNormal, halfwayDir), 0.0f), specularPower);
	float4 specular = specFactor * specularColor;

	float4 lighting = ambient + diffuse + specular;

    // Sample texture
    float4 textureColor = g_texture.SampleLevel(g_sampler, hitTexCoord, 0.0f); // Use t1 for texture

    // Set payload color (just texture for now)
    payload.color = saturate(textureColor * lighting); // Apply lighting to texture color
   }
