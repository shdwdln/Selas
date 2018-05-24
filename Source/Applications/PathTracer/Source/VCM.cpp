
//==============================================================================
// Joe Schutte
//==============================================================================

#include "VCM.h"
#include <Shading/Shading.h>
#include <Shading/SurfaceParameters.h>
#include <Shading/IntegratorContexts.h>
#include <Shading/AreaLighting.h>

#include <SceneLib/SceneResource.h>
#include <SceneLib/ImageBasedLightResource.h>
#include <TextureLib/TextureFiltering.h>
#include <TextureLib/TextureResource.h>
#include <GeometryLib/Camera.h>
#include <GeometryLib/Ray.h>
#include <GeometryLib/SurfaceDifferentials.h>
#include <GeometryLib/HashGrid.h>
#include <UtilityLib/FloatingPoint.h>
#include <MathLib/FloatFuncs.h>
#include <MathLib/FloatStructs.h>
#include <MathLib/IntStructs.h>
#include <MathLib/Trigonometric.h>
#include <MathLib/ImportanceSampling.h>
#include <MathLib/Random.h>
#include <MathLib/Projection.h>
#include <MathLib/Quaternion.h>
#include <ContainersLib/Rect.h>
#include <ContainersLib/CArray.h>
#include <ThreadingLib/Thread.h>
#include <SystemLib/OSThreading.h>
#include <SystemLib/Atomic.h>
#include <SystemLib/MemoryAllocation.h>
#include <SystemLib/Memory.h>
#include <SystemLib/BasicTypes.h>
#include <SystemLib/MinMax.h>
#include <SystemLib/SystemTime.h>

#include <embree3/rtcore.h>
#include <embree3/rtcore_ray.h>

#define MaxBounceCount_         10

#define EnableMultiThreading_   1
#define IntegrationSeconds_     30.0f

#define VcmRadiusFactor_ 0.005f
#define VcmRadiusAlpha_ 0.75f

namespace Shooty
{
    namespace VCM
    {
        //==============================================================================
        struct IntegrationContext
        {
            SceneContext* sceneData;
            RayCastCameraSettings camera;
            uint width;
            uint height;
            uint maxBounceCount;
            float integrationSeconds;
            int64 integrationStartTime;

            float vcmRadius;
            float vcmRadiusAlpha;

            volatile int64* pathsEvaluatedPerPixel;
            volatile int64* completedThreads;
            volatile int64* kernelIndices;
            volatile int64* vcmPassCount;

            void* imageDataSpinlock;
            float3* imageData;
        };

        // JSTODO - This uses a lot of memory. Maybe store hit position and resample the material for each use?
        struct VcmVertex
        {
            float3 throughput;
            uint32 pathLength;
            float dVCM;
            float dVC;
            float dVM;

            SurfaceParameters surface;
        };

        //==============================================================================
        bool OcclusionRay(RTCScene& rtcScene, const SurfaceParameters& surface, float3 direction, float distance)
        {
            float3 origin = OffsetRayOrigin(surface, direction, 0.1f);

            RTCIntersectContext context;
            rtcInitIntersectContext(&context);

            __declspec(align(16)) RTCRay ray;
            ray.org_x = origin.x;
            ray.org_y = origin.y;
            ray.org_z = origin.z;
            ray.dir_x = direction.x;
            ray.dir_y = direction.y;
            ray.dir_z = direction.z;
            ray.tnear = surface.error;
            ray.tfar  = distance;

            rtcOccluded1(rtcScene, &context, &ray);

            // -- ray.tfar == -inf when hit occurs
            return (ray.tfar >= 0.0f);
        }

        //==============================================================================
        bool VcOcclusionRay(RTCScene& rtcScene, const SurfaceParameters& surface, float3 direction, float distance)
        {
            float biasDistance;
            float3 origin = OffsetRayOrigin(surface, direction, 0.1f, biasDistance);

            RTCIntersectContext context;
            rtcInitIntersectContext(&context);

            __declspec(align(16)) RTCRay ray;
            ray.org_x = origin.x;
            ray.org_y = origin.y;
            ray.org_z = origin.z;
            ray.dir_x = direction.x;
            ray.dir_y = direction.y;
            ray.dir_z = direction.z;
            ray.tnear = surface.error;
            ray.tfar = distance - 16.0f * Math::Absf(biasDistance);

            rtcOccluded1(rtcScene, &context, &ray);

            // -- ray.tfar == -inf when hit occurs
            return (ray.tfar >= 0.0f);
        }

        //==============================================================================
        static bool RayPick(const RTCScene& rtcScene, const Ray& ray, HitParameters& hit)
        {
            RTCIntersectContext context;
            rtcInitIntersectContext(&context);

            __declspec(align(16)) RTCRayHit rayhit;
            rayhit.ray.org_x = ray.origin.x;
            rayhit.ray.org_y = ray.origin.y;
            rayhit.ray.org_z = ray.origin.z;
            rayhit.ray.dir_x = ray.direction.x;
            rayhit.ray.dir_y = ray.direction.y;
            rayhit.ray.dir_z = ray.direction.z;
            rayhit.ray.tnear = 0.00001f;
            rayhit.ray.tfar = FloatMax_;

            rayhit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
            rayhit.hit.primID = RTC_INVALID_GEOMETRY_ID;

            rtcIntersect1(rtcScene, &context, &rayhit);

            if(rayhit.hit.geomID == -1)
                return false;

            hit.position.x = rayhit.ray.org_x + rayhit.ray.tfar * ray.direction.x;
            hit.position.y = rayhit.ray.org_y + rayhit.ray.tfar * ray.direction.y;
            hit.position.z = rayhit.ray.org_z + rayhit.ray.tfar * ray.direction.z;
            hit.baryCoords = { rayhit.hit.u, rayhit.hit.v };
            hit.primId = rayhit.hit.primID;

            const float kErr = 32.0f * 1.19209e-07f;
            hit.error = kErr * Max(Max(Math::Absf(hit.position.x), Math::Absf(hit.position.y)), Max(Math::Absf(hit.position.z), rayhit.ray.tfar));

            hit.viewDirection = -ray.direction;
            hit.rxOrigin      = ray.rxOrigin;
            hit.rxDirection   = ray.rxDirection;
            hit.ryOrigin      = ray.ryOrigin;
            hit.ryDirection   = ray.ryDirection;
            hit.pixelIndex    = ray.pixelIndex;
            hit.throughput    = ray.throughput;
            hit.bounceCount   = ray.bounceCount;

            return true;
        }

        //==============================================================================
        static void GenerateLightSample(KernelContext* context, float vcWeight, PathState& state)
        {
            // -- right now we're just generating a sample on the ibl
            float lightSampleWeight = 1.0f;

            LightEmissionSample sample;
            {
                // -- JSTODO - Sample area lights and such
                EmitIblLightSample(context, sample);
            }

            sample.emissionPdfW  *= lightSampleWeight;
            sample.directionPdfA *= lightSampleWeight;

            state.position        = sample.position;
            state.direction       = sample.direction;
            state.throughput      = sample.radiance * (1.0f / sample.emissionPdfW);
            state.dVCM            = sample.directionPdfA / sample.emissionPdfW;
            state.dVC             = sample.cosThetaLight / sample.emissionPdfW;
            state.dVM             = state.dVC * vcWeight;
            state.pathLength      = 1;
            state.isAreaMeasure   = 0; // -- this would be true for any non infinite light source. false since for ibl-only.
        }

        //==============================================================================
        static void GenerateCameraSample(KernelContext* context, uint x, uint y, float lightPathCount, PathState& state)
        {
            const RayCastCameraSettings* __restrict camera = context->camera;

            Ray cameraRay = JitteredCameraRay(camera, context->twister, 0, (float)x, (float)y);

            float cosThetaCamera = Dot(camera->forward, cameraRay.direction);
            float imagePointToCameraDistance = camera->imagePlaneDistance / cosThetaCamera;
            float imageToSolidAngle = imagePointToCameraDistance * imagePointToCameraDistance / cosThetaCamera;

            state.position      = cameraRay.origin;
            state.direction     = cameraRay.direction;
            state.throughput    = float3::One_;

            state.dVCM          = lightPathCount / imageToSolidAngle;
            state.dVC           = 0;
            state.dVM           = 0;
            state.pathLength    = 1;
            state.isAreaMeasure = 1;
        }

        //==============================================================================
        static void ConnectLightPathToCamera(KernelContext* context, PathState& state, const SurfaceParameters& surface, float vmWeight, float lightPathCount)
        {
            const RayCastCameraSettings* __restrict camera = context->camera;

            float3 toPosition = surface.position - camera->position;
            if(Dot(camera->forward, toPosition) <= 0.0f) {
                return;
            }

            int2 imagePosition = WorldToImage(camera, surface.position);
            if(imagePosition.x < 0 || imagePosition.x >= camera->viewportWidth || imagePosition.y < 0 || imagePosition.y >= camera->viewportHeight) {
                return;
            }

            float distance = Length(toPosition);
            toPosition = (1.0f / distance) * toPosition;

            // -- evaluate BSDF
            float bsdfForwardPdf;
            float bsdfReversePdf;
            float3 bsdf = EvaluateBsdf(surface, -state.direction, -toPosition, bsdfForwardPdf, bsdfReversePdf);
            if(bsdf.x == 0 && bsdf.y == 0 && bsdf.z == 0) {
                return;
            }

            float cosThetaSurface = Math::Absf(Dot(surface.geometricNormal, -toPosition));
            float cosThetaCamera  = Dot(camera->forward, toPosition);

            float imagePointToCameraDistance = camera->imagePlaneDistance / cosThetaCamera;
            float imageToSolidAngle = imagePointToCameraDistance * imagePointToCameraDistance / cosThetaCamera;
            float imageToSurface = imageToSolidAngle * cosThetaCamera;
            float surfaceToImage = 1.0f / imageToSurface;

            float cameraPdfA = imageToSurface;

            float lightPartialWeight = (cameraPdfA / lightPathCount) * (vmWeight + state.dVCM + state.dVC * bsdfReversePdf);
            float misWeight = 1.0f / (lightPartialWeight + 1.0f);
            
            float3 pathContribution = misWeight * state.throughput * bsdf * (1.0f / (lightPathCount * surfaceToImage));
            if(pathContribution.x == 0 && pathContribution.y == 0 && pathContribution.z == 0) {
                return;
            }

            if(OcclusionRay(context->sceneData->rtcScene, surface, -toPosition, distance)) {
                uint index = imagePosition.y * context->imageWidth + imagePosition.x;
                context->imageData[index] = context->imageData[index] + pathContribution;
            }
        }

        //==============================================================================
        static float3 ConnectToSkyLight(KernelContext* context, PathState& state)
        {
            float directPdfA;
            float emissionPdfW;
            float3 radiance = DirectIblSample(context, state.direction, directPdfA, emissionPdfW);

            if(state.pathLength == 1) {
                return radiance;
            }

            float cameraWeight = directPdfA * state.dVCM + emissionPdfW * state.dVC;

            float misWeight = 1.0f / (1.0f + cameraWeight);
            return misWeight * radiance;
        }

        //==============================================================================
        static float3 ConnectCameraPathToLight(KernelContext* context, PathState& state, const SurfaceParameters& surface, float vmWeight)
        {
            // -- only using the ibl for now
            float lightSampleWeight = 1.0f;

            // -- choose direction to sample the ibl
            float r0 = Random::MersenneTwisterFloat(context->twister);
            float r1 = Random::MersenneTwisterFloat(context->twister);

            LightDirectSample sample;
            {
                // -- JSTODO - Sample area lights and such
                DirectIblLightSample(context, sample);
                sample.directionPdfA *= lightSampleWeight;
            }
            
            float bsdfForwardPdfW;
            float bsdfReversePdfW;
            float3 bsdf = EvaluateBsdf(surface, -state.direction, sample.direction, bsdfForwardPdfW, bsdfReversePdfW);
            if(bsdf.x == 0 && bsdf.y == 0 && bsdf.z == 0) {
                return float3::Zero_;
            }

            float cosThetaSurface = Math::Absf(Dot(surface.perturbedNormal, sample.direction));

            float lightWeight = bsdfForwardPdfW / sample.directionPdfA;
            float cameraWeight = (sample.emissionPdfW * cosThetaSurface / (sample.directionPdfA * sample.cosThetaLight)) * (vmWeight + state.dVCM + state.dVC * bsdfReversePdfW);

            float misWeight = 1.0f / (lightWeight + 1 + cameraWeight);
            float3 pathContribution = (misWeight * cosThetaSurface / sample.directionPdfA) * sample.radiance * bsdf;
            if(pathContribution.x == 0 && pathContribution.y == 0 && pathContribution.z == 0) {
                return float3::Zero_;
            }

            if(OcclusionRay(context->sceneData->rtcScene, surface, sample.direction, sample.distance)) {
                return pathContribution;
            }

            return float3::Zero_;
        }

        //==============================================================================
        static bool SampleBsdfScattering(KernelContext* context, const SurfaceParameters& surface, float vmWeight, float vcWeight, PathState& pathState)
        {
            BsdfSample sample;
            if(SampleBsdfFunction(context, surface, -pathState.direction, sample) == false) {
                return false;
            }
            if(sample.reflectance.x == 0.0f && sample.reflectance.y == 0.0f && sample.reflectance.z == 0.0f) {
                return false;
            }

            float cosThetaBsdf = Math::Absf(Dot(sample.wi, surface.perturbedNormal));

            pathState.position = surface.position;
            pathState.throughput = pathState.throughput * sample.reflectance;
            pathState.dVC = (cosThetaBsdf / sample.forwardPdfW) * (pathState.dVC * sample.reversePdfW + pathState.dVCM + vmWeight);
            pathState.dVM = (cosThetaBsdf / sample.forwardPdfW) * (pathState.dVM * sample.reversePdfW + pathState.dVCM * vcWeight + 1.0f);
            pathState.dVCM = 1.0f / sample.forwardPdfW;
            pathState.direction = sample.wi;
            ++pathState.pathLength;

            return true;
        }

        //==============================================================================
        static float3 ConnectPathVertices(KernelContext* context, const SurfaceParameters& surface, const PathState& cameraState, const VcmVertex& lightVertex, float vmWeight)
        {
            float3 direction = lightVertex.surface.position - surface.position;
            float distanceSquared = LengthSquared(direction);
            float distance = Math::Sqrtf(distanceSquared);
            direction = (1.0f / distance) * direction;

            float cameraBsdfForwardPdfW;
            float cameraBsdfReversePdfW;
            float3 cameraBsdf = EvaluateBsdf(surface, -cameraState.direction, direction, cameraBsdfForwardPdfW, cameraBsdfReversePdfW);
            if(cameraBsdf.x == 0 && cameraBsdf.y == 0 && cameraBsdf.z == 0) {
                return float3::Zero_;
            }

            float lightBsdfForwardPdfW;
            float lightBsdfReversePdfW;
            float3 lightBsdf = EvaluateBsdf(lightVertex.surface, -direction, lightVertex.surface.view, lightBsdfForwardPdfW, lightBsdfReversePdfW);
            if(lightBsdf.x == 0 && lightBsdf.y == 0 && lightBsdf.z == 0) {
                return float3::Zero_;
            }

            float cosThetaCamera = Math::Absf(Dot(direction, surface.perturbedNormal));
            float cosThetaLight = Math::Absf(Dot(-direction, lightVertex.surface.perturbedNormal));

            float geometryTerm = cosThetaLight * cosThetaCamera / distanceSquared;
            if(geometryTerm < 0.0f) {
                // -- JSTODO - For this to be possible the cosTheta terms would need to be negative. But with transparent surfaces the normal will often be for the other side of the surface.
                return float3::Zero_;
            }

            // -- convert pdfs from solid angle to area measure
            float cameraBsdfPdfA = cameraBsdfForwardPdfW * Math::Absf(cosThetaLight) / distanceSquared;
            float lightBsdfPdfA = lightBsdfForwardPdfW * Math::Absf(cosThetaCamera) / distanceSquared;

            float lightWeight = cameraBsdfPdfA * (vmWeight + lightVertex.dVCM + lightVertex.dVC * lightBsdfReversePdfW);
            float cameraWeight = lightBsdfPdfA * (vmWeight + cameraState.dVCM + cameraState.dVC * cameraBsdfReversePdfW);

            float misWeight = 1.0f / (lightWeight + 1.0f + cameraWeight);

            float3 pathContribution = misWeight * geometryTerm * cameraBsdf * lightBsdf;
            if(pathContribution.x == 0 && pathContribution.y == 0 && pathContribution.z == 0) {
                return float3::Zero_;
            }

            if(VcOcclusionRay(context->sceneData->rtcScene, surface, direction, distance)) {
                return pathContribution;
            }

            return float3::Zero_;
        }

        struct VertexMergingCallbackStruct
        {
            const KernelContext* context;
            const SurfaceParameters* surface;
            const CArray<VcmVertex>* pathVertices;
            const PathState* cameraState;
            float vcWeight;

            float3 result;
        };

        //==============================================================================
        static void MergeVertices(uint vertexIndex, void* userData)
        {
            VertexMergingCallbackStruct* vmData = (VertexMergingCallbackStruct*)userData;
            const KernelContext* context = vmData->context;
            const SurfaceParameters& surface = *vmData->surface;
            const PathState& cameraState = *vmData->cameraState;
            const VcmVertex& lightVertex = vmData->pathVertices->GetData()[vertexIndex];

            if(cameraState.pathLength + lightVertex.pathLength > context->maxPathLength) {
                return;
            }

            float bsdfForwardPdfW;
            float bsdfReversePdfW;
            float3 bsdf = EvaluateBsdf(surface, -cameraState.direction, lightVertex.surface.view, bsdfForwardPdfW, bsdfReversePdfW);
            if(bsdf.x == 0 && bsdf.y == 0 && bsdf.z == 0) {
                return;
            }

            float lightWeight = lightVertex.dVCM * vmData->vcWeight + lightVertex.dVM * bsdfForwardPdfW;
            float cameraWeight = cameraState.dVCM * vmData->vcWeight + cameraState.dVM * bsdfReversePdfW;

            Assert_(!Math::IsNaN(bsdf.x));
            Assert_(!Math::IsNaN(bsdf.y));
            Assert_(!Math::IsNaN(bsdf.z));
            Assert_(!Math::IsNaN(lightVertex.throughput.x));
            Assert_(!Math::IsNaN(lightVertex.throughput.y));
            Assert_(!Math::IsNaN(lightVertex.throughput.z));

            float misWeight = 1.0f / (lightWeight + 1.0f + cameraWeight);
            vmData->result += misWeight * bsdf * lightVertex.throughput;
        }

        //==============================================================================
        static void VertexConnectionAndMerging(KernelContext* context, CArray<VcmVertex>& pathVertices, HashGrid& hashGrid, float kernelRadius, uint width, uint height)
        {
            uint lightPathCount = width * height;

            pathVertices.Clear();
            pathVertices.Reserve((uint32)(lightPathCount));

            CArray<uint32> pathEnds;
            pathEnds.Reserve((uint32)(lightPathCount));
            CArray<float3> deletememaybe;
            deletememaybe.Reserve((uint32)(lightPathCount));

            float kernelRadiusSquare = kernelRadius * kernelRadius;

            float vmNormalization = 1.0f / (Math::Pi_ * kernelRadiusSquare * lightPathCount);

            float vmWeight = Math::Pi_ * kernelRadiusSquare * lightPathCount;
            float vcWeight = 1.0f / vmWeight;

            // -- generate light paths
            for(uint scan = 0; scan < lightPathCount; ++scan) {
                
                // -- create initial light path vertex y_0 
                PathState state;
                GenerateLightSample(context, vcWeight, state);

                while(state.pathLength + 2 < context->maxPathLength) {

                     // JSTODO - Ray storing a pixel index and bounce count was not very forward thinking. Remove those.
                    // -- Make a basic ray. No differentials are used for light path vertices.
                    Ray ray = MakeRay(state.position, state.direction, state.throughput, 0, 0);

                    // -- Cast the ray against the scene
                    HitParameters hit;
                    if(RayPick(context->sceneData->rtcScene, ray, hit) == false) {
                        break;
                    }

                    // -- Calculate all surface information for this hit position
                    SurfaceParameters surface;
                    if(CalculateSurfaceParams(context, &hit, surface) == false) {
                        break;
                    }

                    float connectionLengthSqr = LengthSquared(state.position - surface.position);
                    float absDotNL = Math::Absf(Dot(surface.perturbedNormal, hit.viewDirection));

                    // -- Update accumulated MIS parameters with info from our new hit position
                    if(state.pathLength > 1 || state.isAreaMeasure) {
                        state.dVCM *= connectionLengthSqr;
                    }
                    state.dVCM *= (1.0f / absDotNL);
                    state.dVC  *= (1.0f / absDotNL);
                    state.dVM  *= (1.0f / absDotNL);

                    // -- store the vertex for use with vertex merging
                    VcmVertex vcmVertex;
                    vcmVertex.throughput = state.throughput;
                    vcmVertex.pathLength = state.pathLength;
                    vcmVertex.dVCM = state.dVCM;
                    vcmVertex.dVC = state.dVC;
                    vcmVertex.dVM = state.dVM;
                    vcmVertex.surface = surface;
                    pathVertices.Add(vcmVertex);
                    deletememaybe.Add(surface.position);

                    // -- debug tech
                    //float3 toPosition = surface.position - context->camera->position;
                    //if(Dot(context->camera->forward, toPosition) > 0.0f) {
                    //    int2 imagePosition = WorldToImage(context->camera, surface.position);
                    //    if(imagePosition.x >= 0 && imagePosition.x < context->camera->viewportWidth && imagePosition.y > 0 && imagePosition.y < context->camera->viewportHeight) {

                    //        float3 color;
                    //        if(state.pathLength == 1)
                    //            color = float3(0.0f, 0.0f, 1.0f);
                    //        else if(state.pathLength >= 2 && state.pathLength < 3)
                    //            color = float3(0.0f, 1.0f, 0.0f);
                    //        else
                    //            color = float3(1.0f, 0.0f, 0.0f);
                    //        uint index = imagePosition.y * context->imageWidth + imagePosition.x;
                    //        context->imageData[index] = context->imageData[index] + color;
                    //    }
                    //}

                    // -- connect the path to the camera
                    ConnectLightPathToCamera(context, state, surface, vmWeight, (float)lightPathCount);

                    // -- bsdf scattering to advance the path
                    if(SampleBsdfScattering(context, surface, vmWeight, vcWeight, state) == false) {
                        break;
                    }
                }

                pathEnds.Add(pathVertices.Length());
            }

            // -- build the hash grid
            BuildHashGrid(&hashGrid, lightPathCount, kernelRadius, deletememaybe);

            // -- generate camera paths
            for(uint y = 0; y < height; ++y) {
                for(uint x = 0; x < width; ++x) {
                    uint index = y * width + x;

                    if(x == 830 && y == 375) {
                        index = index;
                    }
                    PathState cameraPathState;
                    GenerateCameraSample(context, x, y, (float)lightPathCount, cameraPathState);

                    float3 color = float3::Zero_;

                    while(cameraPathState.pathLength < context->maxPathLength) {

                        // JSTODO - Ray storing a pixel index and bounce count was not very forward thinking. Remove those.
                       // -- Make a basic ray. No differentials are used atm...
                        Ray ray = MakeRay(cameraPathState.position, cameraPathState.direction, cameraPathState.throughput, 0, 0);

                        // -- Cast the ray against the scene
                        HitParameters hit;
                        if(RayPick(context->sceneData->rtcScene, ray, hit) == false) {
                            // -- if the ray exits the scene then we sample the ibl and accumulate the results.
                            float3 sample = cameraPathState.throughput * ConnectToSkyLight(context, cameraPathState);
                            color += sample;
                            break;
                        }

                        // -- Calculate all surface information for this hit position
                        SurfaceParameters surface;
                        if(CalculateSurfaceParams(context, &hit, surface) == false) {
                            break;
                        }

                        float connectionLengthSqr = LengthSquared(cameraPathState.position - surface.position);
                        float absDotNL = Math::Absf(Dot(surface.geometricNormal, hit.viewDirection));

                        // -- Update accumulated MIS parameters with info from our new hit position
                        cameraPathState.dVCM *= connectionLengthSqr;
                        cameraPathState.dVCM /= absDotNL;
                        cameraPathState.dVC  /= absDotNL;
                        cameraPathState.dVM  /= absDotNL;

                        // -- Vertex connection to a light source
                        if(cameraPathState.pathLength + 1 < context->maxPathLength) {
                            float3 sample = cameraPathState.throughput * ConnectCameraPathToLight(context, cameraPathState, surface, vmWeight);
                            color += sample; 
                        }

                        // -- Vertex connection to a light vertex
                        {
                            uint pathStart = (index == 0) ? 0 : pathEnds[index - 1];
                            uint pathEnd = pathEnds[index];

                            for(uint lightVertexIndex = pathStart; lightVertexIndex < pathEnd; ++lightVertexIndex) {
                                const VcmVertex& lightVertex = pathVertices[lightVertexIndex];
                                if(lightVertex.pathLength + 1 + cameraPathState.pathLength > context->maxPathLength) {
                                    break;
                                }

                                color += cameraPathState.throughput * lightVertex.throughput * ConnectPathVertices(context, surface, cameraPathState, lightVertex, vmWeight);
                            }
                        }

                        // -- Vertex merging
                        {
                            VertexMergingCallbackStruct callbackData;
                            callbackData.context = context;
                            callbackData.surface = &surface;
                            callbackData.pathVertices = &pathVertices;
                            callbackData.cameraState = &cameraPathState;
                            callbackData.vcWeight = vcWeight;
                            callbackData.result = float3::Zero_;
                            SearchHashGrid(&hashGrid, deletememaybe, surface.position, &callbackData, MergeVertices);

                            color += cameraPathState.throughput * vmNormalization * callbackData.result;
                        }

                        // -- bsdf scattering to advance the path
                        if(SampleBsdfScattering(context, surface, vmWeight, vcWeight, cameraPathState) == false) {
                            break;
                        }
                    }

                    context->imageData[index] += color;
                }
            }
        }

        //==============================================================================
        static void VCMKernel(void* userData)
        {
            IntegrationContext* integratorContext = static_cast<IntegrationContext*>(userData);
            int64 kernelIndex = Atomic::Increment64(integratorContext->kernelIndices);

            Random::MersenneTwister twister;
            Random::MersenneTwisterInitialize(&twister, (uint32)kernelIndex);

            uint width = integratorContext->width;
            uint height = integratorContext->height;

            float3* imageData = AllocArrayAligned_(float3, width * height, CacheLineSize_);
            Memory::Zero(imageData, sizeof(float3) * width * height);

            KernelContext kernelContext;
            kernelContext.sceneData        = integratorContext->sceneData;
            kernelContext.camera           = &integratorContext->camera;
            kernelContext.imageData        = imageData;
            kernelContext.imageWidth       = width;
            kernelContext.imageHeight      = height;
            kernelContext.twister          = &twister;
            kernelContext.maxPathLength    = integratorContext->maxBounceCount;
            kernelContext.rayStackCapacity = 1024 * 1024;
            kernelContext.rayStackCount    = 0;
            kernelContext.rayStack         = AllocArrayAligned_(Ray, kernelContext.rayStackCapacity, CacheLineSize_);

            HashGrid hashGrid;
            CArray<VcmVertex> lightVertices;

            int64 pathsTracedPerPixel = 0;
            float elapsedMs = 0.0f;
            while(elapsedMs < integratorContext->integrationSeconds) {
                int64 index = Atomic::Increment64(integratorContext->vcmPassCount);
                float iterationIndex = index + 1.0f;

                float vcmKernelRadius = integratorContext->vcmRadius / Math::Powf(iterationIndex, 0.5f * (1.0f - integratorContext->vcmRadiusAlpha));

                VertexConnectionAndMerging(&kernelContext, lightVertices, hashGrid, vcmKernelRadius, width, height);
                ++pathsTracedPerPixel;

                int64 startTime = integratorContext->integrationStartTime;
                elapsedMs = SystemTime::ElapsedMs(startTime) / 1000.0f;
            }

            ShutdownHashGrid(&hashGrid);
            lightVertices.Close();

            Atomic::Add64(integratorContext->pathsEvaluatedPerPixel, pathsTracedPerPixel);

            FreeAligned_(kernelContext.rayStack);
            Random::MersenneTwisterShutdown(&twister);

            EnterSpinLock(integratorContext->imageDataSpinlock);

            float3* resultImageData = integratorContext->imageData;
            for(uint y = 0; y < height; ++y) {
                for(uint x = 0; x < width; ++x) {
                    uint index = y * width + x;
                    resultImageData[index] += imageData[index];
                }
            }

            LeaveSpinLock(integratorContext->imageDataSpinlock);

            FreeAligned_(imageData);
            Atomic::Increment64(integratorContext->completedThreads);
        }

        //==============================================================================
        void GenerateImage(SceneContext& context, uint width, uint height, float3* imageData)
        {
            const SceneResource* scene = context.scene;
            SceneResourceData* sceneData = scene->data;

            RayCastCameraSettings camera;
            InitializeRayCastCamera(scene->data->camera, width, height, camera);

            int64 completedThreads = 0;
            int64 kernelIndex = 0;
            int64 pathsEvaluatedPerPixel = 0;
            int64 vcmPassCount = 0;

            #if EnableMultiThreading_ 
                const uint additionalThreadCount = 7;
            #else
                const uint additionalThreadCount = 0;
            #endif

            IntegrationContext integratorContext;
            integratorContext.sceneData              = &context;
            integratorContext.camera                 = camera;
            integratorContext.imageData              = imageData;
            integratorContext.width                  = width;
            integratorContext.height                 = height;
            integratorContext.maxBounceCount         = MaxBounceCount_;
            SystemTime::GetCycleCounter(&integratorContext.integrationStartTime);
            integratorContext.integrationSeconds     = IntegrationSeconds_;
            integratorContext.pathsEvaluatedPerPixel = &pathsEvaluatedPerPixel;
            integratorContext.vcmPassCount           = &vcmPassCount;
            integratorContext.completedThreads       = &completedThreads;
            integratorContext.kernelIndices          = &kernelIndex;
            integratorContext.imageDataSpinlock      = CreateSpinLock();
            integratorContext.vcmRadius              = VcmRadiusFactor_ * sceneData->boundingSphere.w;
            integratorContext.vcmRadiusAlpha         = VcmRadiusAlpha_;

            #if EnableMultiThreading_
                ThreadHandle threadHandles[additionalThreadCount];

                // -- fork threads
                for(uint scan = 0; scan < additionalThreadCount; ++scan) {
                    threadHandles[scan] = CreateThread(VCMKernel, &integratorContext);
                }
            #endif

            // -- do work on the main thread too
            VCMKernel(&integratorContext);

            #if EnableMultiThreading_ 
                // -- wait for any other threads to finish
                while(*integratorContext.completedThreads != *integratorContext.kernelIndices);

                for(uint scan = 0; scan < additionalThreadCount; ++scan) {
                    ShutdownThread(threadHandles[scan]);
                }
            #endif

            CloseSpinlock(integratorContext.imageDataSpinlock);

            for(uint y = 0; y < height; ++y) {
                for(uint x = 0; x < width; ++x) {
                    uint index = y * width + x;
                    imageData[index] = imageData[index] * (1.0f / pathsEvaluatedPerPixel);
                }
            }
        }
    }
}