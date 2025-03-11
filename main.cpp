// Copyright 2023 Stefan Zellmann and Jefferson Amstutz
// SPDX-License-Identifier: Apache-2.0

// std
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <iostream>
#include <numeric>
#include <random>
#include <sstream>
#include <thread>
// anari
#define ANARI_EXTENSION_UTILITY_IMPL
#include <anari/anari_cpp.hpp>
#include <anari/anari_cpp/ext/linalg.h>
// stb_image
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

using namespace anari::math;

// ========================================================
// Log ANARI errors
// ========================================================
static void statusFunc(const void * /*userData*/,
    ANARIDevice /*device*/,
    ANARIObject source,
    ANARIDataType /*sourceType*/,
    ANARIStatusSeverity severity,
    ANARIStatusCode /*code*/,
    const char *message)
{
  if (severity == ANARI_SEVERITY_FATAL_ERROR) {
    fprintf(stderr, "[FATAL][%p] %s\n", source, message);
    std::exit(1);
  } else if (severity == ANARI_SEVERITY_ERROR) {
    fprintf(stderr, "[ERROR][%p] %s\n", source, message);
  } else if (severity == ANARI_SEVERITY_WARNING) {
    fprintf(stderr, "[WARN ][%p] %s\n", source, message);
  } else if (severity == ANARI_SEVERITY_PERFORMANCE_WARNING) {
    fprintf(stderr, "[PERF ][%p] %s\n", source, message);
  }
  // Ignore INFO/DEBUG messages
}

// ========================================================
// query anari extensions
// ========================================================
static bool deviceHasExtension(anari::Library library,
    const std::string &deviceSubtype,
    const std::string &extName)
{
  const char **extensions =
      anariGetDeviceExtensions(library, deviceSubtype.c_str());

  for (; *extensions; extensions++) {
    if (*extensions == extName)
      return true;
  }
  return false;
}

// ========================================================
// generate our test scene
// ========================================================
void initializeWorld(anari::Device device, anari::World world, const float3 &pos)
{
  const uint32_t numSpheres = 10000;
  const float radius = .015f;

  std::mt19937 rng;
  rng.seed(0);
  std::normal_distribution<float> vert_dist(0.f, 0.25f);

  // Create + fill position and color arrays with randomized values //

  auto indicesArray = anari::newArray1D(device, ANARI_UINT32, numSpheres);
  auto positionsArray =
      anari::newArray1D(device, ANARI_FLOAT32_VEC3, numSpheres);
  auto distanceArray = anari::newArray1D(device, ANARI_FLOAT32, numSpheres);
  {
    auto *positions = anari::map<float3>(device, positionsArray);
    auto *distances = anari::map<float>(device, distanceArray);
    for (uint32_t i = 0; i < numSpheres; i++) {
      const auto a = positions[i][0] = vert_dist(rng);
      const auto b = positions[i][1] = vert_dist(rng);
      const auto c = positions[i][2] = vert_dist(rng);
      distances[i] = std::sqrt(a * a + b * b + c * c); // will be roughly 0-1
      // translate
      positions[i] += pos;
    }
    anari::unmap(device, positionsArray);
    anari::unmap(device, distanceArray);

    auto *indicesBegin = anari::map<uint32_t>(device, indicesArray);
    auto *indicesEnd = indicesBegin + numSpheres;
    std::iota(indicesBegin, indicesEnd, 0);
    std::shuffle(indicesBegin, indicesEnd, rng);
    anari::unmap(device, indicesArray);
  }

  // Create and parameterize geometry //

  auto geometry = anari::newObject<anari::Geometry>(device, "sphere");
  anari::setAndReleaseParameter(
      device, geometry, "primitive.index", indicesArray);
  anari::setAndReleaseParameter(
      device, geometry, "vertex.position", positionsArray);
  anari::setAndReleaseParameter(
      device, geometry, "vertex.attribute0", distanceArray);
  anari::setParameter(device, geometry, "radius", radius);
  anari::commitParameters(device, geometry);

  // Create color map texture //

  auto texelArray = anari::newArray1D(device, ANARI_FLOAT32_VEC3, 2);
  {
    auto *texels = anari::map<float3>(device, texelArray);
    texels[0][0] = 1.f;
    texels[0][1] = 0.f;
    texels[0][2] = 0.f;
    texels[1][0] = 0.f;
    texels[1][1] = 1.f;
    texels[1][2] = 0.f;
    anari::unmap(device, texelArray);
  }

  auto texture = anari::newObject<anari::Sampler>(device, "image1D");
  anari::setAndReleaseParameter(device, texture, "image", texelArray);
  anari::setParameter(device, texture, "filter", "linear");
  anari::commitParameters(device, texture);

  // Create and parameterize material //

  auto material = anari::newObject<anari::Material>(device, "matte");
  anari::setAndReleaseParameter(device, material, "color", texture);
  anari::commitParameters(device, material);

  // Create and parameterize surface //

  auto surface = anari::newObject<anari::Surface>(device);
  anari::setAndReleaseParameter(device, surface, "geometry", geometry);
  anari::setAndReleaseParameter(device, surface, "material", material);
  anari::commitParameters(device, surface);

  // Create and parameterize world //

#if 1
  {
    auto surfaceArray = anari::newArray1D(device, ANARI_SURFACE, 1);
    auto *s = anari::map<anari::Surface>(device, surfaceArray);
    s[0] = surface;
    anari::unmap(device, surfaceArray);
    anari::setAndReleaseParameter(device, world, "surface", surfaceArray);
  }
#else
  anari::setAndReleaseParameter(
      device, world, "surface", anari::newArray1D(device, &surface));
#endif
  anari::release(device, surface);
  anari::commitParameters(device, world);

  // Add a directional light source //

  auto light = anari::newObject<anari::Light>(device, "directional");
  anari::setParameterArray1D(device, world, "light", &light, 1);
  anari::release(device, light);
}

// ========================================================
// Function to initialize a renderer
// ========================================================
static void initializeRenderer(anari::Device device, anari::Renderer renderer)
{
  const float4 backgroundColor = {0.1f, 0.1f, 0.1f, 1.f};
  anari::setParameter(device, renderer, "background", backgroundColor);
  anari::setParameter(device, renderer, "pixelSamples", 1);
  anari::commitParameters(device, renderer);
}

// ========================================================
// Function to initialize a camera
// ========================================================
static void initializeCamera(anari::Device device, anari::Camera camera)
{
  anari::setParameter(device, camera, "position", float3(1.5f, 1.68f, 1.5f));
  anari::setParameter(device, camera, "direction", float3(0, 0, -1));
  anari::setParameter(device, camera, "up", float3(0, 1, 0));
  anari::commitParameters(device, camera);
}

// ========================================================
// Function to initialize a frame
// ========================================================
static void initializeFrame(anari::Device device,
                            anari::Frame frame,
                            anari::World world,
                            anari::Renderer renderer,
                            anari::Camera camera)
{
  uint2 imageSize = {1024, 1024};
  anari::setParameter(device, frame, "size", imageSize);
  anari::setParameter(device, frame, "channel.color", ANARI_UFIXED8_RGBA_SRGB);

  anari::setParameter(device, frame, "world", world);
  anari::setParameter(device, frame, "renderer", renderer);
  anari::setParameter(device, frame, "camera", camera);
  anari::commitParameters(device, frame);
}

// ========================================================
// Function to render a given frame (renderer+world+cam)
//  and (optionally) produce an output image
// ========================================================
static void render(
    anari::Device device, anari::Frame frame, const std::string &fileName)
{
  // Render frame and print out duration property //

  anari::render(device, frame);
  anari::wait(device, frame);

  float duration = 0.f;
  anari::getProperty(device, frame, "duration", duration, ANARI_NO_WAIT);

  printf("rendered frame in %fms\n", duration * 1000);

  if (!fileName.empty()) {
    stbi_flip_vertically_on_write(1);
    auto fb = anari::map<uint32_t>(device, frame, "channel.color");
    stbi_write_png(
        fileName.c_str(), fb.width, fb.height, 4, fb.data, 4 * fb.width);
    anari::unmap(device, frame, "channel.color");

    std::cout << "Output: " << fileName << '\n';
  }
}

int main()
{
  // Setup ANARI device //

  auto library = anari::loadLibrary("environment", statusFunc);
  auto device = anari::newDevice(library, "default");

  // Create world from a helper function //

  anari::World world = anari::newObject<anari::World>(device);
  std::thread initWorldThread([&]() {
    initializeWorld(device, world, float3(1.5f, 1.5f, 0.f));
    fprintf(stdout, "%s\n", "world initialization thread finished");
  });

  // Create renderer //
  anari::Renderer renderer = anari::newObject<anari::Renderer>(device, "default");
  std::thread initRendererThread([&]() {
    initializeRenderer(device, renderer);
    fprintf(stdout, "%s\n", "renderer initialization thread finished");
  });

  // Create camera //

  auto camera = anari::newObject<anari::Camera>(device, "perspective");
  std::thread initCameraThread([&]() {
    initializeCamera(device, camera);
    fprintf(stdout, "%s\n", "camera initialization thread finished");
  });

  // Create frame (top-level object) //

  auto frame = anari::newObject<anari::Frame>(device);
  std::thread initFrameThread([&]() {
    initializeFrame(device, frame, world, renderer, camera);
    fprintf(stdout, "%s\n", "frame initialization thread finished");
  });

  // Periodically query some extensions //

  std::atomic<bool> finish_queryExtension{false};
  std::thread queryExtensionThread([&]() {
    for (;;) {
      bool res
          = deviceHasExtension(library, "default", "ANARI_KHR_CAMERA_PERSPECTIVE");
      if (!res) {
        fprintf(stderr, "%s\n", "extension not found");
      }

      if (finish_queryExtension)
        break;
    }
    fprintf(stdout, "%s\n", "extension query thread finished");
  });

  // Periodically query world boudns //

  std::atomic<bool> finish_queryBoundsNoWait{false};
  std::thread queryBoundsNoWaitThread([&]() {
    for (;;) {
      float bounds[6] = { 1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f };
      anari::getProperty(device, world, "bounds", bounds, ANARI_NO_WAIT);

      if (finish_queryBoundsNoWait)
        break;
    }
    fprintf(stdout, "%s\n", "bounds query (no wait) thread finished");
  });

  std::atomic<bool> finish_queryBoundsWait{false};
  std::thread queryBoundsWaitThread([&]() {
    for (;;) {
      float bounds[6] = { 1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f };
      anari::getProperty(device, world, "bounds", bounds, ANARI_WAIT);

      if (finish_queryBoundsWait)
        break;
    }
    fprintf(stdout, "%s\n", "bounds query (wait) thread finished");
  });

  // Rendering //
  std::thread renderThread([&]() {
    for (int i=0; i<10; i++) {
      std::stringstream str;
      str << "out_" << i << ".png";
      render(device, frame, str.str());
    }

    // Tell the periodic query threads to finish:
    finish_queryExtension = true;
    finish_queryBoundsNoWait = true;
    finish_queryBoundsWait = true;

    fprintf(stdout, "%s\n", "render thread finished");
  });

  // Join all threads //

  renderThread.join();
  queryBoundsWaitThread.join();
  queryBoundsNoWaitThread.join();
  queryExtensionThread.join();
  initWorldThread.join();
  initRendererThread.join();
  initCameraThread.join();
  initFrameThread.join();

  // Cleanup remaining ANARI objets //

  anari::release(device, camera);
  anari::release(device, renderer);
  anari::release(device, world);
  anari::release(device, frame);
  anari::release(device, device);

  anari::unloadLibrary(library);

  return 0;
}
