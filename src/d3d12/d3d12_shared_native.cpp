#ifndef _WIN32

#include "d3d12_shared_native.hpp"
#include "dxmt_format.hpp"
#include "log/log.hpp"
#include "d3d11_4.h"

#include <errno.h>
#include <fcntl.h>
#include <mutex>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_set>

#include <directx-native/dxn_embedder.h>
#include <directx-native/dxn_shm.h>

namespace dxmt {

bool
IsShareableTexture(WMT::Device device, const D3D12_RESOURCE_DESC &desc) {
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.MipLevels > 1 || desc.DepthOrArraySize != 1 ||
      desc.SampleDesc.Count > 1 || desc.Layout == D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE ||
      desc.Layout == D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE)
    return false;
  MTL_DXGI_FORMAT_DESC format;
  return SUCCEEDED(MTLQueryDXGIFormat(device, desc.Format, format)) && format.PixelFormat != WMTPixelFormatInvalid &&
         format.BytesPerTexel &&
         !(format.Flag & (MTL_DXGI_FORMAT_BC | MTL_DXGI_FORMAT_DEPTH_PLANER | MTL_DXGI_FORMAT_STENCIL_PLANER));
}

HRESULT
AllocateShmTexture(
    WMT::Device device, const D3D12_RESOURCE_DESC &desc, const WMTTextureInfo &info, const dxn_shared_texture *import,
    Rc<Texture> &out_texture, dxn_shared_texture &out_pod
) {
  if (!IsShareableTexture(device, desc)) {
    ERR("shm texture: unsupported texture (format ", desc.Format, ", mips ", desc.MipLevels, ", array ",
        desc.DepthOrArraySize, ", samples ", desc.SampleDesc.Count, ")");
    return E_INVALIDARG;
  }
  MTL_DXGI_FORMAT_DESC format;
  MTLQueryDXGIFormat(device, desc.Format, format);

  uint64_t stride, logical_size, offset = 0;
  if (import) {
    /* The producer's layout is the contract. Every field is untrusted: validate without overflowing (stride *
     * Height can wrap) and within the range the Texture constructor truncates to. */
    stride = import->planes[0].pitch;
    logical_size = import->size;
    offset = import->planes[0].offset;
    if (!stride || !desc.Height || stride > UINT32_MAX || logical_size > UINT32_MAX ||
        stride < desc.Width * format.BytesPerTexel || logical_size / stride < desc.Height) {
      ERR("shm texture: import descriptor stride/size inconsistent");
      return E_INVALIDARG;
    }
  } else {
    stride = align(desc.Width * format.BytesPerTexel,
                   (uint64_t)device.minimumLinearTextureAlignmentForPixelFormat(format.PixelFormat));
    logical_size = stride * desc.Height;
    if (logical_size > UINT32_MAX)
      return E_INVALIDARG;
  }

  /* The surface may start part-way into the object (placed in a shared heap); mmap wants a page-aligned file
   * offset, so map from the page floor and place the texture at the delta. */
  const uint64_t map_floor = offset & ~(dxn_shm_page_align(1) - 1);
  const uint64_t delta = offset - map_floor;
  if (delta > UINT32_MAX || logical_size > UINT64_MAX - delta)
    return E_INVALIDARG;
  const uint64_t mapped_size = dxn_shm_page_align(delta + logical_size);

  int fd = import ? import->fd : dxn_shm_create(mapped_size);
  if (fd < 0) {
    ERR("shm texture: shm_open failed: ", errno);
    return E_FAIL;
  }
  if (import) {
    /* Pages past the object's end raise SIGBUS: the mapping must be backed in full. */
    struct stat st;
    if (fstat(fd, &st) != 0 || (uint64_t)st.st_size < map_floor + mapped_size) {
      ERR("shm texture: shm object smaller than descriptor size");
      return E_INVALIDARG;
    }
  }
  void *mapped = mmap(nullptr, mapped_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)map_floor);
  if (mapped == MAP_FAILED) {
    ERR("shm texture: mmap failed: ", errno);
    if (!import)
      close(fd);
    return E_FAIL;
  }

  /* A buffer over the mapping; its deallocator munmaps, so the memory lives as long as the last Metal reference. */
  WMTBufferInfo buffer_info;
  buffer_info.length = mapped_size;
  buffer_info.options = WMTResourceHazardTrackingModeUntracked; /* StorageModeShared */
  buffer_info.memory.set(mapped);
  buffer_info.gpu_address = 0;
  auto buffer = device.newBufferMapped(buffer_info);
  if (!buffer) {
    ERR("shm texture: newBufferWithBytesNoCopy failed");
    munmap(mapped, mapped_size);
    if (!import)
      close(fd);
    return E_FAIL;
  }

  WMTTextureInfo linear_info = info;
  linear_info.mach_port = 0;
  linear_info.options = WMTResourceHazardTrackingModeUntracked;
  /* Never readonly: another process writes it. */
  Flags<TextureAllocationFlag> flags;
  flags.set(TextureAllocationFlag::Shared);
  Rc<Texture> texture = new Texture((unsigned)logical_size, (unsigned)stride, linear_info, device);
  auto allocation = texture->allocateFromBuffer(std::move(buffer), (uint8_t *)mapped + delta, (unsigned)delta, flags);
  if (!allocation->texture()) {
    ERR("shm texture: linear texture creation failed (stride ", stride, ")");
    if (!import)
      close(fd);
    return E_FAIL;
  }
  texture->rename(std::move(allocation));

  /* The descriptor owns a fd of its own both ways, so an opened texture can be shared again and outlive the
   * descriptor it was opened from. */
  int pod_fd = import ? fcntl(fd, F_DUPFD_CLOEXEC, 0) : fd;
  if (pod_fd < 0) {
    ERR("shm texture: dup failed: ", errno);
    return E_FAIL;
  }
  out_pod = {};
  DXN_SHARED_INIT(&out_pod, DXN_SHARED_TEXTURE_MAGIC);
  out_pod.fd = pod_fd;
  out_pod.meta.width = (uint32_t)desc.Width;
  out_pod.meta.height = desc.Height;
  out_pod.meta.mip_levels = 1;
  out_pod.meta.array_size = 1;
  out_pod.meta.dxgi_format = desc.Format;
  out_pod.meta.sample_count = 1;
  out_pod.meta.usage = D3D11_USAGE_DEFAULT;
  out_pod.meta.bind_flags = 0;
  if (!(desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
    out_pod.meta.bind_flags |= D3D11_BIND_SHADER_RESOURCE;
  if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)
    out_pod.meta.bind_flags |= D3D11_BIND_RENDER_TARGET;
  if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
    out_pod.meta.bind_flags |= D3D11_BIND_UNORDERED_ACCESS;
  out_pod.meta.misc_flags = D3D11_RESOURCE_MISC_SHARED;
  out_pod.plane_count = 1;
  out_pod.planes[0].offset = offset;
  out_pod.planes[0].pitch = stride;
  out_pod.size = logical_size;
  out_texture = std::move(texture);
  return S_OK;
}

HRESULT
DescFromSharedTexture(const dxn_shared_texture &pod, D3D12_RESOURCE_DESC &desc) {
  if (pod.plane_count != 1 || pod.fd < 0 || !pod.meta.width || !pod.meta.height)
    return E_INVALIDARG;
  desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = pod.meta.width;
  desc.Height = pod.meta.height;
  desc.DepthOrArraySize = (UINT16)pod.meta.array_size;
  desc.MipLevels = (UINT16)pod.meta.mip_levels;
  desc.Format = (DXGI_FORMAT)pod.meta.dxgi_format;
  desc.SampleDesc.Count = pod.meta.sample_count;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  if (pod.meta.bind_flags & D3D11_BIND_RENDER_TARGET)
    desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  if (pod.meta.bind_flags & D3D11_BIND_UNORDERED_ACCESS)
    desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  return S_OK;
}

/* The NT-style handles vended and not closed yet: dxmt_shared_handle_close takes only these. */
static std::mutex vended_lock;
static std::unordered_set<void *> vended;

HANDLE
VendSharedHandle(const dxn_shared_texture &pod) {
  int fd = fcntl(pod.fd, F_DUPFD_CLOEXEC, 0);
  if (fd < 0)
    return nullptr;
  auto handle = (dxn_shared_texture *)malloc(sizeof(dxn_shared_texture));
  if (!handle) {
    close(fd);
    return nullptr;
  }
  *handle = pod;
  handle->fd = fd;
  std::lock_guard<std::mutex> lock(vended_lock);
  vended.insert(handle);
  return handle;
}

} // namespace dxmt

/* The embedder's CloseHandle for NT-style shared handles (dxn_embedder.h). */
extern "C" __attribute__((visibility("default"))) int dxmt_shared_handle_close(void *handle);

extern "C" int
dxmt_shared_handle_close(void *handle) {
  {
    std::lock_guard<std::mutex> lock(dxmt::vended_lock);
    if (!handle || !dxmt::vended.erase(handle))
      return -EINVAL;
  }
  auto pod = (dxn_shared_texture *)handle;
  if (pod->fd >= 0)
    close(pod->fd);
  free(pod);
  return 0;
}

#endif /* !_WIN32 */
