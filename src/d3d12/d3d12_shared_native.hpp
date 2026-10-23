/*
 * Native (non-Wine) shared D3D12 textures, over anonymous POSIX shm as the
 * d3d11 ones are (d3d11_texture_shared_native.hpp).
 *
 * A committed texture created with D3D12_HEAP_FLAG_SHARED is one linear
 * plane of shared memory: ID3D12Device::CreateSharedHandle vends an
 * NT-style dxn_shared_texture with a fd of its own, which the embedder
 * closes with dxmt_shared_handle_close, and OpenSharedHandle maps one
 * (a d3d11 producer's included).  The descriptor's meta is d3d11's
 * (D3D11_TEXTURE2D_DESC), whichever API exported it.
 */

#pragma once

#ifndef _WIN32

#include "d3d12_device.hpp"
#include "dxmt_texture.hpp"

#include <directx-native/dxn_shared.h>

namespace dxmt {

/* Whether a texture desc fits a linear shm surface: 2D, one subresource,
 * no MSAA, an ordinary color format. */
bool IsShareableTexture(WMT::Device device, const D3D12_RESOURCE_DESC &desc);

/*
 * Build the linear shm-backed texture for `desc` (`info` from
 * PopulateWMTTextureInfo).  import == nullptr: a fresh shm object
 * (producer); otherwise map import's fd (not consumed) at its recorded
 * pitch, size and offset (consumer).  On success out_pod is filled and owns
 * a NEW fd (close it when the resource dies), and out_texture holds the
 * texture with the allocation renamed in.
 */
HRESULT AllocateShmTexture(
    WMT::Device device, const D3D12_RESOURCE_DESC &desc, const WMTTextureInfo &info, const dxn_shared_texture *import,
    Rc<Texture> &out_texture, dxn_shared_texture &out_pod
);

/* The D3D12 desc of a descriptor's texture (meta in d3d11's terms). */
HRESULT DescFromSharedTexture(const dxn_shared_texture &pod, D3D12_RESOURCE_DESC &desc);

/* An NT-style handle for `pod`: a copy with a dup of its fd, which
 * dxmt_shared_handle_close closes and frees. */
HANDLE VendSharedHandle(const dxn_shared_texture &pod);

} // namespace dxmt

#endif /* !_WIN32 */
