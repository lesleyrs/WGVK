// raytracing.c – minimal AABB ray tracing example with working procedural hit group

#include "common.h"
#include <wgvk.h>
#include <wgvk_structs_impl.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#define WIDTH  512
#define HEIGHT 512

// -----------------------------------------------------------------------------
// GLSL shaders
// -----------------------------------------------------------------------------

// Ray generation shader: simple pinhole camera
static const char raygenSource[] = R"(
#version 460
#extension GL_EXT_ray_tracing : require

layout(binding = 0) uniform accelerationStructureEXT topLevelAS;
layout(binding = 1, rgba32f) uniform image2D image;

layout(binding = 2) uniform CameraProperties {
    vec4 eye;
    vec4 target;
    vec4 up;
    vec4 fovY; // .x = vertical FOV in radians
} camera;

layout(location = 0) rayPayloadEXT vec4 payload;

void main() {
    vec2 pixelCenter = vec2(gl_LaunchIDEXT.xy) + vec2(0.5);
    vec2 uv = pixelCenter / vec2(gl_LaunchSizeEXT.xy);
    vec2 d = uv * 2.0 - 1.0;

    vec3 origin = camera.eye.xyz;
    vec3 target = camera.target.xyz;
    vec3 forward = normalize(target - origin);
    vec3 right = normalize(cross(normalize(camera.up.xyz), forward));
    vec3 up = normalize(cross(forward, right));

    float f = tan(camera.fovY.x * 0.5);
    vec3 dir = normalize(forward + f * d.x * right + f * d.y * up);

    payload = vec4(0.0);

    traceRayEXT(
        topLevelAS,
        gl_RayFlagsOpaqueEXT,
        0xFF,
        0,          // sbtRecordOffset
        0,          // sbtRecordStride
        0,          // missIndex
        origin,
        0.001,
        dir,
        100.0,
        0           // payload location
    );

    imageStore(image, ivec2(gl_LaunchIDEXT.xy), vec4(payload.xyz, 1.0));
}
)";

// Procedural intersection shader for AABB [0,0,0] – [1,1,1] in object space
static const char intersectSource[] = R"(
#version 460
#extension GL_EXT_ray_tracing : require

hitAttributeEXT vec3 attribs;

// Ray-box intersection with axis-aligned box [0,0,0] to [1,1,1]
bool intersectBox(vec3 orig, vec3 dir, out float tHit) {
    vec3 invD = 1.0 / dir;

    vec3 t0 = (vec3(0.0) - orig) * invD;
    vec3 t1 = (vec3(1.0) - orig) * invD;

    vec3 tmin = min(t0, t1);
    vec3 tmax = max(t0, t1);

    float tEnter = max(max(tmin.x, tmin.y), tmin.z);
    float tExit  = min(min(tmax.x, tmax.y), tmax.z);

    tHit = tEnter;
    return (tExit >= max(tEnter, 0.0));
}

void main() {
    vec3 orig = gl_ObjectRayOriginEXT;
    vec3 dir  = gl_ObjectRayDirectionEXT;
    float tHit;
    if (intersectBox(orig - vec3(0, gl_PrimitiveID * 1.5, 0), dir, tHit)) {
        attribs = vec3(orig + tHit * dir); // Not used, but must be written
        reportIntersectionEXT(tHit, 0);
    }
}
)";

// Closest hit shader: simple lambert shading and instance visualization
static const char rchitSource[] = R"(
#version 460
#extension GL_EXT_ray_tracing : require

layout(location = 0) rayPayloadInEXT vec4 payload;
hitAttributeEXT vec3 attribs;

void main() {
    // Color by instance (0 = red, 1 = green, etc.)
    vec3 baseColor;
    if (gl_InstanceID == 0) {
        baseColor = vec3(1.0, 0.2, 0.2);
    } else if (gl_InstanceID == 1) {
        baseColor = vec3(0.2, 1.0, 0.2);
    } else {
        baseColor = vec3(0.7);
    }

    vec3 N = normalize(vec3(0.0, 1.0, 0.0));
    vec3 L = normalize(vec3(1.0, 1.0, 1.0));
    float diff = max(dot(N, L), 0.2);

    payload = vec4(attribs,1);
}
)";

// Miss shader: dark blue-ish background
static const char rmissSource[] = R"(
#version 460
#extension GL_EXT_ray_tracing : require

layout(location = 0) rayPayloadInEXT vec4 payload;

void main() {
    vec3 dir = normalize(gl_WorldRayDirectionEXT);
    float t = 0.5 * (dir.y + 1.0);
    vec3 sky = mix(vec3(0.1, 0.1, 0.15), vec3(0.2, 0.4, 0.8), t);
    payload = vec4(sky, 1.0);
}
)";

// -----------------------------------------------------------------------------
// Helper: compile GLSL into a WGPUShaderModule
// -----------------------------------------------------------------------------
static WGPUShaderModule compileGLSLModule(WGPUDevice device, const char *source, WGPUShaderStage stage) {
    WGPUShaderSourceGLSL glslSource = {
        .chain.sType = WGPUSType_ShaderSourceGLSL,
        .code = {
            source,
            WGPU_STRLEN
        },
        .stage = stage
    };
    WGPUShaderModuleDescriptor md = {
        .nextInChain = &glslSource.chain
    };
    return wgpuDeviceCreateShaderModule(device, &md);
}

// Simple camera struct for the UBO
typedef struct Vector4 {
    float x, y, z, w;
}Vector4;

int main(void) {
    // -------------------------------------------------------------------------
    // Init
    // -------------------------------------------------------------------------
    wgpu_base base = wgpu_init();
    WGPUDevice device = base.device;
    WGPUQueue queue = base.queue;

    // -------------------------------------------------------------------------
    // BLAS: single AABB [0,0,0]–[1,1,1]
    // -------------------------------------------------------------------------
    float aabb[12] = {
        0.0f, 0.0f, 0.0f,
        1.0f, 1.0f, 1.0f,
        0.0f, 1.5f, 0.0f,
        1.0f, 2.5f, 1.0f,
    };

    WGPUBuffer aabbBuffer = wgpuDeviceCreateBuffer(device, &(const WGPUBufferDescriptor){
        .usage = WGPUBufferUsage_Raytracing | WGPUBufferUsage_ShaderDeviceAddress,
        .size  = sizeof(aabb)
    });

    wgpuQueueWriteBuffer(queue, aabbBuffer, 0, aabb, sizeof(aabb));

    WGPURayTracingAccelerationGeometryDescriptor geometry = {
        .type = WGPURayTracingAccelerationGeometryType_AABBs,
        .index.format = WGPUIndexFormat_Undefined,
        .aabb = {
            .buffer = aabbBuffer,
            .count  = 2,
            .stride = sizeof(float) * 6,
            .offset = 0
        }
    };

    WGPURayTracingAccelerationContainerDescriptor blasDesc = {
        .level = WGPURayTracingAccelerationContainerLevel_Bottom,
        .geometryCount = 1,
        .geometries = &geometry
    };

    WGPURayTracingAccelerationContainer blas =
        wgpuDeviceCreateRayTracingAccelerationContainer(device, &blasDesc);

    // Build BLAS
    {
        WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(device, NULL);
        wgpuCommandEncoderBuildRayTracingAccelerationContainer(enc, blas);
        WGPUCommandBuffer cbuf = wgpuCommandEncoderFinish(enc, NULL);
        wgpuQueueSubmit(queue, 1, &cbuf);
        wgpuCommandEncoderRelease(enc);
        wgpuCommandBufferRelease(cbuf);
    }

    // -------------------------------------------------------------------------
    // TLAS: one instance of the BLAS (you can easily add more)
    // -------------------------------------------------------------------------
    WGPUTransformMatrix identity = {
        1,0,0,0,
        0,1,0,0,
        0,0,1,0
    };
    WGPUTransformMatrix translated = {
        1,0,0,2,
        0,1,0,0,
        0,0,1,0
    };

    WGPURayTracingAccelerationInstanceDescriptor instances[2] = {
        {
            .usage = WGPURayTracingAccelerationInstanceUsage_TriangleCullDisable,
            .instanceId = 0,
            .instanceOffset = 0,
            .mask = 0xFF,
            .transformMatrix = identity,
            .geometryContainer = blas
        },
        {
            .usage = WGPURayTracingAccelerationInstanceUsage_TriangleCullDisable,
            .instanceId = 1,
            .instanceOffset = 0,
            .mask = 0xFF,
            .transformMatrix = translated,
            .geometryContainer = blas
        }
    };
    

    WGPURayTracingAccelerationContainerDescriptor tlasDesc = {
        .level = WGPURayTracingAccelerationContainerLevel_Top,
        .instanceCount = 1,
        .instances = instances
    };

    WGPURayTracingAccelerationContainer tlas =
        wgpuDeviceCreateRayTracingAccelerationContainer(device, &tlasDesc);

    // Build TLAS
    {
        WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(device, NULL);
        wgpuCommandEncoderBuildRayTracingAccelerationContainer(enc, tlas);
        WGPUCommandBuffer cbuf = wgpuCommandEncoderFinish(enc, NULL);
        wgpuQueueSubmit(queue, 1, &cbuf);
        wgpuCommandEncoderRelease(enc);
        wgpuCommandBufferRelease(cbuf);
        wgpuQueueWaitIdle(queue);
    }

    // -------------------------------------------------------------------------
    // Shader modules
    // -------------------------------------------------------------------------
    WGPUShaderModule raygenModule     = compileGLSLModule(device, raygenSource,    WGPUShaderStage_RayGen);
    WGPUShaderModule intersectModule  = compileGLSLModule(device, intersectSource, WGPUShaderStage_Intersect);
    WGPUShaderModule rchitModule      = compileGLSLModule(device, rchitSource,     WGPUShaderStage_ClosestHit);
    WGPUShaderModule rmissModule      = compileGLSLModule(device, rmissSource,     WGPUShaderStage_Miss);

    // -------------------------------------------------------------------------
    // Shader Binding Table (stages + groups)
    // Order of stages defines indices used in groups
    // -------------------------------------------------------------------------
    WGPURayTracingShaderBindingTableStageDescriptor stages[4] = {
        { .stage = WGPUShaderStage_RayGen,     .module = raygenModule    }, // 0
        { .stage = WGPUShaderStage_Miss,       .module = rmissModule     }, // 1
        { .stage = WGPUShaderStage_ClosestHit, .module = rchitModule     }, // 2
        { .stage = WGPUShaderStage_Intersect,  .module = intersectModule }  // 3
    };

    WGPURayTracingShaderBindingTableGroupDescriptor groups[3] = {
        // Group 0: RayGen
        {
            .type = WGPURayTracingShaderBindingTableGroupType_General,
            .generalIndex     = 0,
            .closestHitIndex  = -1,
            .anyHitIndex      = -1,
            .intersectionIndex= -1
        },
        // Group 1: Procedural hit group (AABB)
        {
            .type = WGPURayTracingShaderBindingTableGroupType_ProceduralHitGroup,
            .generalIndex     = -1,
            .closestHitIndex  =  2, // rchit stage
            .anyHitIndex      = -1,
            .intersectionIndex=  3  // intersect stage
        },
        // Group 2: Miss
        {
            .type = WGPURayTracingShaderBindingTableGroupType_General,
            .generalIndex     = 1, // miss stage
            .closestHitIndex  = -1,
            .anyHitIndex      = -1,
            .intersectionIndex= -1
        }
    };

    WGPURayTracingShaderBindingTableDescriptor sbtDesc = {
        .stageCount = 4,
        .stages     = stages,
        .groupCount = 3,
        .groups     = groups
    };

    WGPURayTracingShaderBindingTable sbt = wgpuDeviceCreateRayTracingShaderBindingTable(device, &sbtDesc);

    // -------------------------------------------------------------------------
    // Ray tracing pipeline
    // -------------------------------------------------------------------------
    WGPUBindGroupLayoutEntry bglEntries[3] = {
        {
            .binding = 0,
            .visibility = WGPUShaderStage_RayGen | WGPUShaderStage_ClosestHit | WGPUShaderStage_Miss,
            .accelerationStructure = 1
        },
        {
            .binding = 1,
            .visibility = WGPUShaderStage_RayGen,
            .storageTexture = {
                .viewDimension = WGPUTextureViewDimension_2D,
                .access = WGPUStorageTextureAccess_WriteOnly,
                .format = WGPUTextureFormat_RGBA32Float
            }
        },
        {
            .binding = 2,
            .visibility = WGPUShaderStage_RayGen,
            .buffer = {
                .type = WGPUBufferBindingType_Uniform,
                .minBindingSize = sizeof(struct Vector4) * 4,
                .hasDynamicOffset = false
            }
        }
    };

    WGPUBindGroupLayoutDescriptor bglDesc = {
        .entries = bglEntries,
        .entryCount = 3
    };

    WGPUBindGroupLayout bgl = wgpuDeviceCreateBindGroupLayout(device, &bglDesc);

    WGPUPipelineLayoutDescriptor plDesc = {
        .bindGroupLayoutCount = 1,
        .bindGroupLayouts = &bgl
    };

    WGPUPipelineLayout plLayout = wgpuDeviceCreatePipelineLayout(device, &plDesc);

    WGPURayTracingStateDescriptor rtState = {
        .shaderBindingTable = sbt,
        .maxPayloadSize = 16,    // vec4 payload
        .maxRecursionDepth = 1
    };

    WGPURayTracingPipelineDescriptor rtpDesc = {
        .rayTracingState = rtState,
        .layout = plLayout
    };

    WGPURaytracingPipeline rtPipeline =
        wgpuDeviceCreateRayTracingPipeline(device, &rtpDesc);

    // -------------------------------------------------------------------------
    // Storage texture + camera UBO
    // -------------------------------------------------------------------------
    const WGPUTextureFormat storageTextureFormat = WGPUTextureFormat_RGBA32Float;

    WGPUTextureDescriptor texDesc = {
        .usage = WGPUTextureUsage_StorageBinding | WGPUTextureUsage_CopySrc,
        .dimension = WGPUTextureDimension_2D,
        .size = {
            .width = WIDTH,
            .height = HEIGHT,
            .depthOrArrayLayers = 1
        },
        .format = storageTextureFormat,
        .mipLevelCount = 1,
        .sampleCount = 1,
        .viewFormatCount = 1,
        .viewFormats = &storageTextureFormat
    };

    WGPUTexture storageTexture = wgpuDeviceCreateTexture(device, &texDesc);

    WGPUTextureViewDescriptor viewDesc = {
        .format = storageTextureFormat,
        .dimension = WGPUTextureViewDimension_2D,
        .baseMipLevel = 0,
        .mipLevelCount = 1,
        .baseArrayLayer = 0,
        .arrayLayerCount = 1,
        .aspect = WGPUTextureAspect_All,
        .usage = WGPUTextureUsage_StorageBinding | WGPUTextureUsage_CopySrc
    };

    WGPUTextureView storageView = wgpuTextureCreateView(storageTexture, &viewDesc);

    WGPUBuffer cameraBuffer = wgpuDeviceCreateBuffer(device, &(const WGPUBufferDescriptor){
        .size = sizeof(struct Vector4) * 4,
        .usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst
    });

    struct Vector4 cameraData[4] = {
        { 0.0f, 1.0f, -4.0f, 0.0f },  // eye
        { 0.0f, 0.0f,  0.0f, 0.0f },  // target
        { 0.0f, 1.0f,  0.0f, 0.0f },  // up
        { 1.1f, 0.0f,  0.0f, 0.0f }   // fovY.x = ~63deg in radians
    };

    wgpuQueueWriteBuffer(queue, cameraBuffer, 0, cameraData, sizeof(cameraData));

    WGPUBindGroupEntry bgEntries[3] = {
        {
            .binding = 0,
            .accelerationStructure = tlas
        },
        {
            .binding = 1,
            .textureView = storageView
        },
        {
            .binding = 2,
            .buffer = cameraBuffer,
            .size = WGPU_WHOLE_SIZE
        }
    };

    WGPUBindGroupDescriptor bgDesc = {
        .layout = bgl,
        .entries = bgEntries,
        .entryCount = 3
    };

    WGPUBindGroup bindGroup = wgpuDeviceCreateBindGroup(device, &bgDesc);

    // -------------------------------------------------------------------------
    // Ray tracing pass
    // -------------------------------------------------------------------------
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(device, NULL);

    WGPURayTracingPassDescriptor rtPassDesc = {
        .maxRecursionDepth = 1,
        .maxPayloadSize = 16,
        .shaderBindingTable = sbt
    };

    WGPURaytracingPassEncoder rtenc =
        wgpuCommandEncoderBeginRaytracingPass(enc, &rtPassDesc);

    wgpuRaytracingPassEncoderSetPipeline(rtenc, rtPipeline);
    wgpuRaytracingPassEncoderSetBindGroup(rtenc, 0, bindGroup, 0, NULL);
    // Group indices: 0 = raygen, 1 = hit, 2 = miss
    wgpuRaytracingPassEncoderTraceRays(rtenc, 0, 1, 2, WIDTH, HEIGHT, 1);
    wgpuRaytracingPassEncoderEnd(rtenc);

    WGPUCommandBuffer cbuf = wgpuCommandEncoderFinish(enc, NULL);
    wgpuQueueSubmit(queue, 1, &cbuf);
    wgpuCommandEncoderRelease(enc);
    wgpuCommandBufferRelease(cbuf);
    wgpuQueueWaitIdle(queue);

    // -------------------------------------------------------------------------
    // Readback: copy texture -> buffer, map, write PNG
    // -------------------------------------------------------------------------
    size_t pixelCount = (size_t)WIDTH * (size_t)HEIGHT;
    size_t floatPixelSize = 4 * sizeof(float);
    size_t bufferSize = pixelCount * floatPixelSize;

    WGPUBuffer readback = wgpuDeviceCreateBuffer(device, &(const WGPUBufferDescriptor){
        .size = bufferSize,
        .usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst
    });

    {
        WGPUTexelCopyTextureInfo src = {
            .texture = storageTexture,
            .mipLevel = 0,
            .origin = { 0, 0, 0 },
            .aspect = WGPUTextureAspect_All
        };

        WGPUTexelCopyBufferInfo dst = {
            .buffer = readback,
            .layout = {
                .bytesPerRow = WIDTH * floatPixelSize,
                .rowsPerImage = HEIGHT,
                .offset = 0
            }
        };

        WGPUCommandEncoder dumpEnc = wgpuDeviceCreateCommandEncoder(device, NULL);
        WGPUExtent3D copySize = { WIDTH, HEIGHT, 1 };

        wgpuCommandEncoderCopyTextureToBuffer(dumpEnc, &src, &dst, &copySize);
        WGPUCommandBuffer dumpBuf = wgpuCommandEncoderFinish(dumpEnc, NULL);
        wgpuQueueSubmit(queue, 1, &dumpBuf);
        wgpuCommandEncoderRelease(dumpEnc);
        wgpuCommandBufferRelease(dumpBuf);
        wgpuQueueWaitIdle(queue);
    }

    struct Float4 { float x, y, z, w; } *mapPtr = NULL;
    wgpuBufferMap(readback, WGPUMapMode_Read, 0, WGPU_WHOLE_MAP_SIZE, (void **)&mapPtr);

    struct RGBA8 { uint8_t r, g, b, a; };
    struct RGBA8 *img = calloc(pixelCount, sizeof(struct RGBA8));

    for (size_t i = 0; i < pixelCount; i++) {
        float r = mapPtr[i].x;
        float g = mapPtr[i].y;
        float b = mapPtr[i].z;
        if (r < 0.0f) r = 0.0f; if (r > 1.0f) r = 1.0f;
        if (g < 0.0f) g = 0.0f; if (g > 1.0f) g = 1.0f;
        if (b < 0.0f) b = 0.0f; if (b > 1.0f) b = 1.0f;

        img[i].r = (uint8_t)(255.0f * r);
        img[i].g = (uint8_t)(255.0f * g);
        img[i].b = (uint8_t)(255.0f * b);
        img[i].a = 255;
    }

    stbi_write_png("rt_aabb.png", WIDTH, HEIGHT, 4, img, WIDTH * 4);

    free(img);
    // (Cleanup of WGPU objects omitted for brevity.)

    return 0;
}
