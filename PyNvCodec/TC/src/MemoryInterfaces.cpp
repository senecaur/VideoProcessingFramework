/*
 * Copyright 2019 NVIDIA Corporation
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *    http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "MemoryInterfaces.hpp"
#include <cstring>
#include <cuda_runtime.h>
#include <new>
#include <sstream>

using namespace VPF;
using namespace VPF;
using namespace std;

#ifdef TRACK_TOKEN_ALLOCATIONS
#include <algorithm>
#include <atomic>
#include <iostream>
#include <mutex>
#include <sstream>
#include <vector>

namespace VPF {
static auto ThrowOnCudaError = [](CUresult res, int lineNum = -1) {
  if (CUDA_SUCCESS != res) {
    stringstream ss;

    if (lineNum > 0) {
      ss << __FILE__ << ":";
      ss << lineNum << endl;
    }

    const char *errName = nullptr;
    if (CUDA_SUCCESS != cuGetErrorName(res, &errName)) {
      ss << "CUDA error with code " << res << endl;
    } else {
      ss << "CUDA error: " << errName << endl;
    }

    const char *errDesc = nullptr;
    if (CUDA_SUCCESS != cuGetErrorString(res, &errDesc)) {
      ss << "No error string available" << endl;
    } else {
      ss << errDesc << endl;
    }

    throw runtime_error(ss.str());
  }
};

struct AllocInfo {
  uint64_t id;
  uint64_t size;

  bool operator==(const AllocInfo &other) {
    /* Buffer size may change during the lifetime so we check id only;
     */
    return id == other.id;
  }

  AllocInfo(decltype(id) const &newId, decltype(size) const &newSize)
      : id(newId), size(newSize) {}
};

struct AllocRegister {
  vector<AllocInfo> instances;
  mutex guard;
  uint64_t ID = 0U;

  decltype(AllocInfo::id) AddNote(decltype(AllocInfo::size) const &size) {
    unique_lock<decltype(guard)> lock;
    auto id = ID++;
    AllocInfo info(id, size);
    instances.push_back(info);
    return id;
  }

  void DeleteNote(AllocInfo const &allocInfo) {
    unique_lock<decltype(guard)> lock;
    instances.erase(remove(instances.begin(), instances.end(), allocInfo),
                    instances.end());
  }

  /* Call this after you're done releasing mem objects in your app;
   */
  size_t GetSize() const { return instances.size(); }

  /* Call this after you're done releasing mem objects in your app;
   */
  AllocInfo const *GetNoteByIndex(uint64_t idx) {
    return idx < instances.size() ? instances.data() + idx : nullptr;
  }
};

AllocRegister BuffersRegister, HWSurfaceRegister;

bool CheckAllocationCounters() {
  auto numLeakedBuffers = BuffersRegister.GetSize();
  auto numLeakedSurfaces = HWSurfaceRegister.GetSize();

  if (numLeakedBuffers) {
    cerr << "Leaked buffers (id : size): " << endl;
    for (auto i = 0; i < numLeakedBuffers; i++) {
      auto pNote = BuffersRegister.GetNoteByIndex(i);
      cerr << "\t" << pNote->id << "\t: " << pNote->size << endl;
    }
  }

  if (numLeakedSurfaces) {
    cerr << "Leaked surfaces (id : size): " << endl;
    for (auto i = 0; i < numLeakedSurfaces; i++) {
      auto pNote = HWSurfaceRegister.GetNoteByIndex(i);
      cerr << "\t" << pNote->id << "\t: " << pNote->size << endl;
    }
  }

  return (0U == numLeakedBuffers) && (0U == numLeakedSurfaces);
}
} // namespace VPF
#endif

Buffer *Buffer::Make(size_t bufferSize) {
  return new Buffer(bufferSize, false);
}

Buffer *Buffer::Make(size_t bufferSize, void *pCopyFrom) {
  return new Buffer(bufferSize, pCopyFrom, false);
}

Buffer::Buffer(size_t bufferSize, bool ownMemory)
    : mem_size(bufferSize), own_memory(ownMemory) {
  if (own_memory) {
    if (!Allocate()) {
      throw bad_alloc();
    }
  }
#ifdef TRACK_TOKEN_ALLOCATIONS
  id = BuffersRegister.AddNote(mem_size);
#endif
}

Buffer::Buffer(size_t bufferSize, void *pCopyFrom, bool ownMemory)
    : mem_size(bufferSize), own_memory(ownMemory) {
  if (own_memory) {
    if (Allocate()) {
      memcpy(this->GetRawMemPtr(), pCopyFrom, bufferSize);
    } else {
      throw bad_alloc();
    }
  } else {
    pRawData = pCopyFrom;
  }
#ifdef TRACK_TOKEN_ALLOCATIONS
  id = BuffersRegister.AddNote(mem_size);
#endif
}

Buffer::~Buffer() {
  Deallocate();
#ifdef TRACK_TOKEN_ALLOCATIONS
  AllocInfo info(id, mem_size);
  BuffersRegister.DeleteNote(info);
#endif
}

size_t Buffer::GetRawMemSize() { return mem_size; }

bool Buffer::Allocate() {
  if (GetRawMemSize()) {
    cudaMallocHost(&pRawData, GetRawMemSize());
    return (nullptr != pRawData);
  }
  return true;
}

void Buffer::Deallocate() {
  if (own_memory) {
    cudaFreeHost(pRawData);
  }
  pRawData = nullptr;
}

void *Buffer::GetRawMemPtr() { return pRawData; }

void Buffer::Update(size_t newSize, void *newPtr) {
  Deallocate();

  mem_size = newSize;
  if (own_memory) {
    Allocate();
    if (newPtr) {
      memcpy(GetRawMemPtr(), newPtr, newSize);
    }
  } else {
    pRawData = newPtr;
  }
}

Buffer *Buffer::MakeOwnMem(size_t bufferSize) {
  return new Buffer(bufferSize, true);
}

SurfacePlane::SurfacePlane() = default;

SurfacePlane &SurfacePlane::operator=(const SurfacePlane &other) {
  Deallocate();

  ownMem = false;
  gpuMem = other.gpuMem;
  width = other.width;
  height = other.height;
  pitch = other.pitch;
  elemSize = other.elemSize;

  return *this;
}

SurfacePlane::SurfacePlane(const SurfacePlane &other)
    : ownMem(false), gpuMem(other.gpuMem), width(other.width),
      height(other.height), pitch(other.pitch), elemSize(other.elemSize) {}

SurfacePlane::SurfacePlane(uint32_t newWidth, uint32_t newHeight,
                           uint32_t newPitch, uint32_t newElemSize,
                           CUdeviceptr pNewPtr)
    : ownMem(false), gpuMem(pNewPtr), width(newWidth), height(newHeight),
      pitch(newPitch), elemSize(newElemSize) {}

SurfacePlane::SurfacePlane(uint32_t newWidth, uint32_t newHeight,
                           uint32_t newElemSize, CUcontext context)
    : ownMem(true), width(newWidth), height(newHeight), elemSize(newElemSize),
      ctx(context) {
  Allocate();
}

SurfacePlane::~SurfacePlane() { Deallocate(); }

void SurfacePlane::Allocate() {
  if (!OwnMemory()) {
    return;
  }

  size_t newPitch;
  CudaCtxPush ctxPush(ctx);
  auto res = cuMemAllocPitch(&gpuMem, &newPitch, width * elemSize, height, 16);
  //ThrowOnCudaError(res, __LINE__);
  pitch = newPitch;

#ifdef TRACK_TOKEN_ALLOCATIONS
  id = HWSurfaceRegister.AddNote(GpuMem());
#endif
}

void SurfacePlane::Deallocate() {
  if (!OwnMemory()) {
    return;
  }

#ifdef TRACK_TOKEN_ALLOCATIONS
  AllocInfo info(id, GpuMem());
  HWSurfaceRegister.DeleteNote(info);
#endif

  CudaCtxPush ctxPush(ctx);
  cuMemFree(gpuMem);
}

Surface::Surface() = default;

Surface::~Surface() = default;

Surface *Surface::Make(Pixel_Format format) {
  switch (format) {
  case Y:
    return new SurfaceY;
  case RGB:
    return new SurfaceRGB;
  case NV12:
    return new SurfaceNV12;
  case YUV420:
    return new SurfaceYUV420;
  case RGB_PLANAR:
    return new SurfaceRGBPlanar;
  default:
    return nullptr;
  }
}

Surface *Surface::Make(Pixel_Format format, uint32_t newWidth,
                       uint32_t newHeight, CUcontext context) {
  switch (format) {
  case Y:
    return new SurfaceY(newWidth, newHeight, context);
  case NV12:
    return new SurfaceNV12(newWidth, newHeight, context);
  case YUV420:
    return new SurfaceYUV420(newWidth, newHeight, context);
  case RGB:
    return new SurfaceRGB(newWidth, newHeight, context);
  case RGB_PLANAR:
    return new SurfaceRGBPlanar(newWidth, newHeight, context);
  default:
    return nullptr;
  }
}

SurfaceY::~SurfaceY() = default;

SurfaceY::SurfaceY() = default;

Surface *SurfaceY::Clone() { return new SurfaceY(*this); }

Surface *SurfaceY::Create() { return new SurfaceY; }

SurfaceY::SurfaceY(const SurfaceY &other) : plane(other.plane) {}

SurfaceY::SurfaceY(uint32_t width, uint32_t height, CUcontext context)
    : plane(width, height, ElemSize(), context) {}

SurfaceY &SurfaceY::operator=(const SurfaceY &other) {
  plane = other.plane;
  return *this;
}

uint32_t SurfaceY::Width(uint32_t planeNumber) const {
  if (planeNumber < NumPlanes()) {
    return plane.Width();
  }

  throw invalid_argument("Invalid plane number");
}

uint32_t SurfaceY::WidthInBytes(uint32_t planeNumber) const {
  if (planeNumber < NumPlanes()) {
    return plane.Width() * plane.ElemSize();
  }

  throw invalid_argument("Invalid plane number");
}

uint32_t SurfaceY::Height(uint32_t planeNumber) const {
  if (planeNumber < NumPlanes()) {
    return plane.Height();
  }

  throw invalid_argument("Invalid plane number");
}

uint32_t SurfaceY::Pitch(uint32_t planeNumber) const {
  if (planeNumber < NumPlanes()) {
    return plane.Pitch();
  }

  throw invalid_argument("Invalid plane number");
}

uint32_t SurfaceY::HostMemSize() const { return plane.GetHostMemSize(); }

CUdeviceptr SurfaceY::PlanePtr(uint32_t planeNumber) {
  if (planeNumber < NumPlanes()) {
    return plane.GpuMem();
  }

  throw invalid_argument("Invalid plane number");
}

void SurfaceY::Update(SurfacePlane &newPlane) { plane = newPlane; }

SurfacePlane *SurfaceY::GetSurfacePlane(uint32_t planeNumber) {
  return planeNumber ? nullptr : &plane;
}

SurfaceNV12::~SurfaceNV12() = default;

SurfaceNV12::SurfaceNV12() = default;

SurfaceNV12::SurfaceNV12(const SurfaceNV12 &other) : plane(other.plane) {}

SurfaceNV12::SurfaceNV12(uint32_t width, uint32_t height, CUcontext context)
    : plane(width, height * 3 / 2, ElemSize(), context) {}

SurfaceNV12 &SurfaceNV12::operator=(const SurfaceNV12 &other) {
  plane = other.plane;
  return *this;
}

Surface *SurfaceNV12::Clone() { return new SurfaceNV12(*this); }

Surface *SurfaceNV12::Create() { return new SurfaceNV12; }

uint32_t SurfaceNV12::Width(uint32_t planeNumber) const {
  switch (planeNumber) {
  case 0:
  case 1:
    return plane.Width();
  default:
    break;
  }
  throw invalid_argument("Invalid plane number");
}

uint32_t SurfaceNV12::WidthInBytes(uint32_t planeNumber) const {
  switch (planeNumber) {
  case 0:
  case 1:
    return plane.Width() * plane.ElemSize();
  default:
    break;
  }
  throw invalid_argument("Invalid plane number");
}

uint32_t SurfaceNV12::Height(uint32_t planeNumber) const {
  switch (planeNumber) {
  case 0:
    return plane.Height() * 2 / 3;
  case 1:
    return plane.Height() / 3;
  default:
    break;
  }
  throw invalid_argument("Invalid plane number");
}

uint32_t SurfaceNV12::Pitch(uint32_t planeNumber) const {
  switch (planeNumber) {
  case 0:
  case 1:
    return plane.Pitch();
  default:
    break;
  }
  throw invalid_argument("Invalid plane number");
}

uint32_t SurfaceNV12::HostMemSize() const { return plane.GetHostMemSize(); }

CUdeviceptr SurfaceNV12::PlanePtr(uint32_t planeNumber) {
  switch (planeNumber) {
  case 0:
    return plane.GpuMem();
  case 1:
    return plane.GpuMem() + Height() * Pitch();
  default:
    break;
  }
  throw invalid_argument("Invalid plane number");
}

void SurfaceNV12::Update(SurfacePlane &newPlane) { plane = newPlane; }

SurfacePlane *SurfaceNV12::GetSurfacePlane(uint32_t planeNumber) {
  return planeNumber ? nullptr : &plane;
}

SurfaceYUV420::~SurfaceYUV420() = default;

SurfaceYUV420::SurfaceYUV420() = default;

SurfaceYUV420::SurfaceYUV420(const SurfaceYUV420 &other)
    : planeY(other.planeY), planeU(other.planeU), planeV(other.planeV) {}

SurfaceYUV420::SurfaceYUV420(uint32_t width, uint32_t height, CUcontext context)
    : planeY(width, height, ElemSize(), context),
      planeU(width / 2, height / 2, ElemSize(), context),
      planeV(width / 2, height / 2, ElemSize(), context) {}

SurfaceYUV420 &SurfaceYUV420::operator=(const SurfaceYUV420 &other) {
  planeY = other.planeY;
  planeU = other.planeU;
  planeV = other.planeV;

  return *this;
}

Surface *SurfaceYUV420::Clone() { return new SurfaceYUV420(*this); }

Surface *SurfaceYUV420::Create() { return new SurfaceYUV420; }

uint32_t SurfaceYUV420::Width(uint32_t planeNumber) const {
  switch (planeNumber) {
  case 0:
    return planeY.Width();
  case 1:
    return planeU.Width();
  case 2:
    return planeV.Width();
  default:
    break;
  }
  throw invalid_argument("Invalid plane number");
}

uint32_t SurfaceYUV420::WidthInBytes(uint32_t planeNumber) const {
  switch (planeNumber) {
  case 0:
    return planeY.Width() * planeY.ElemSize();
  case 1:
    return planeU.Width() * planeU.ElemSize();
  case 2:
    return planeV.Width() * planeV.ElemSize();
  default:
    break;
  }
  throw invalid_argument("Invalid plane number");
}

uint32_t SurfaceYUV420::Height(uint32_t planeNumber) const {
  switch (planeNumber) {
  case 0:
    return planeY.Height();
  case 1:
    return planeU.Height();
  case 2:
    return planeV.Height();
  default:
    break;
  }
  throw invalid_argument("Invalid plane number");
}

uint32_t SurfaceYUV420::Pitch(uint32_t planeNumber) const {
  switch (planeNumber) {
  case 0:
    return planeY.Pitch();
  case 1:
    return planeU.Pitch();
  case 2:
    return planeV.Pitch();
  default:
    break;
  }
  throw invalid_argument("Invalid plane number");
}

uint32_t SurfaceYUV420::HostMemSize() const {
  return planeY.GetHostMemSize() + planeU.GetHostMemSize() +
         planeV.GetHostMemSize();
}

CUdeviceptr SurfaceYUV420::PlanePtr(uint32_t planeNumber) {
  switch (planeNumber) {
  case 0:
    return planeY.GpuMem();
  case 1:
    return planeU.GpuMem();
  case 2:
    return planeV.GpuMem();
  default:
    break;
  }
  throw invalid_argument("Invalid plane number");
}

void SurfaceYUV420::Update(SurfacePlane &newPlaneY, SurfacePlane &newPlaneU,
                           SurfacePlane &newPlaneV) {
  planeY = newPlaneY;
  planeU = newPlaneY;
  planeV = newPlaneV;
}

SurfacePlane *SurfaceYUV420::GetSurfacePlane(uint32_t planeNumber) {
  switch (planeNumber) {
  case 0U:
    return &planeY;
  case 1U:
    return &planeU;
  case 2U:
    return &planeV;
  default:
    return nullptr;
  }
}

SurfaceRGB::~SurfaceRGB() = default;

SurfaceRGB::SurfaceRGB() = default;

SurfaceRGB::SurfaceRGB(const SurfaceRGB &other) : plane(other.plane) {}

SurfaceRGB::SurfaceRGB(uint32_t width, uint32_t height, CUcontext context)
    : plane(width * 3, height, ElemSize(), context) {}

SurfaceRGB &SurfaceRGB::operator=(const SurfaceRGB &other) {
  plane = other.plane;
  return *this;
}

Surface *SurfaceRGB::Clone() { return new SurfaceRGB(*this); }

Surface *SurfaceRGB::Create() { return new SurfaceRGB; }

uint32_t SurfaceRGB::Width(uint32_t planeNumber) const {
  if (planeNumber < NumPlanes()) {
    return plane.Width() / 3;
  }

  throw invalid_argument("Invalid plane number");
}

uint32_t SurfaceRGB::WidthInBytes(uint32_t planeNumber) const {
  if (planeNumber < NumPlanes()) {
    return plane.Width() * plane.ElemSize();
  }

  throw invalid_argument("Invalid plane number");
}

uint32_t SurfaceRGB::Height(uint32_t planeNumber) const {
  if (planeNumber < NumPlanes()) {
    return plane.Height();
  }

  throw invalid_argument("Invalid plane number");
}

uint32_t SurfaceRGB::Pitch(uint32_t planeNumber) const {
  if (planeNumber < NumPlanes()) {
    return plane.Pitch();
  }

  throw invalid_argument("Invalid plane number");
}

uint32_t SurfaceRGB::HostMemSize() const { return plane.GetHostMemSize(); }

CUdeviceptr SurfaceRGB::PlanePtr(uint32_t planeNumber) {
  if (planeNumber < NumPlanes()) {
    return plane.GpuMem();
  }

  throw invalid_argument("Invalid plane number");
}

void SurfaceRGB::Update(SurfacePlane &newPlane) { plane = newPlane; }

SurfacePlane *SurfaceRGB::GetSurfacePlane(uint32_t planeNumber) {
  return planeNumber ? nullptr : &plane;
}

SurfaceRGBPlanar::~SurfaceRGBPlanar() = default;

SurfaceRGBPlanar::SurfaceRGBPlanar() = default;

SurfaceRGBPlanar::SurfaceRGBPlanar(const SurfaceRGBPlanar &other)
    : plane(other.plane) {}

SurfaceRGBPlanar::SurfaceRGBPlanar(uint32_t width, uint32_t height,
                                   CUcontext context)
    : plane(width, height * 3, ElemSize(), context) {}

SurfaceRGBPlanar &SurfaceRGBPlanar::operator=(const SurfaceRGBPlanar &other) {
  plane = other.plane;
  return *this;
}

Surface *SurfaceRGBPlanar::Clone() { return new SurfaceRGBPlanar(*this); }

Surface *SurfaceRGBPlanar::Create() { return new SurfaceRGBPlanar; }

uint32_t SurfaceRGBPlanar::Width(uint32_t planeNumber) const {
  if (planeNumber < NumPlanes()) {
    return plane.Width();
  }

  throw invalid_argument("Invalid plane number");
}

uint32_t SurfaceRGBPlanar::WidthInBytes(uint32_t planeNumber) const {
  if (planeNumber < NumPlanes()) {
    return plane.Width() * plane.ElemSize();
  }

  throw invalid_argument("Invalid plane number");
}

uint32_t SurfaceRGBPlanar::Height(uint32_t planeNumber) const {
  if (planeNumber < NumPlanes()) {
    return plane.Height() / 3;
  }

  throw invalid_argument("Invalid plane number");
}

uint32_t SurfaceRGBPlanar::Pitch(uint32_t planeNumber) const {
  if (planeNumber < NumPlanes()) {
    return plane.Pitch();
  }

  throw invalid_argument("Invalid plane number");
}

uint32_t SurfaceRGBPlanar::HostMemSize() const {
  return plane.GetHostMemSize();
}

CUdeviceptr SurfaceRGBPlanar::PlanePtr(uint32_t planeNumber) {
  if (planeNumber < NumPlanes()) {
    return plane.GpuMem();
  }

  throw invalid_argument("Invalid plane number");
}

void SurfaceRGBPlanar::Update(SurfacePlane &newPlane) { plane = newPlane; }

SurfacePlane *SurfaceRGBPlanar::GetSurfacePlane(uint32_t planeNumber) {
  return planeNumber ? nullptr : &plane;
}
