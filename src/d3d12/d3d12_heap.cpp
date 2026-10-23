/*
 * Copyright 2026 Feifan He for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "d3d12_device.hpp"
#include "d3d12_pageable.hpp"
#include "com/com_pointer.hpp"
#ifdef DXMT_NATIVE
#include <unistd.h>
#endif

namespace dxmt {

class MTLD3D12HeapImpl : public MTLD3D12Pageable<MTLD3D12Heap> {

  D3D12_HEAP_DESC desc_;

public:
  MTLD3D12HeapImpl(MTLD3D12Device *pDevice) : MTLD3D12Pageable<MTLD3D12Heap>(pDevice) {}

  ~MTLD3D12HeapImpl() {
    if (heap)
      device_->UnregisterResidency(heap);
    heap = {};
#ifdef DXMT_NATIVE
    if (external_fd >= 0)
      close(external_fd);
#endif
  }

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12Heap)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12Resource), riid)) {
      WARN("D3D12Heap: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  }

  HRESULT
  Initialize(const D3D12_HEAP_DESC *pDesc) {
    if (pDesc->Flags & D3D12_HEAP_FLAG_ALLOW_DISPLAY)
      return E_INVALIDARG; // must be committed resource

    desc_ = *pDesc;
    desc_.Properties.CreationNodeMask = 1;
    desc_.Properties.VisibleNodeMask = 1;


    switch (pDesc->Alignment) {
    case 0:
      desc_.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
      [[fallthrough]];
    case D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT:
    case D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT:
      break;
    default:
      return E_INVALIDARG;
    }

    auto size_aligned = align(pDesc->SizeInBytes, desc_.Alignment);
    if (!size_aligned)
      return E_INVALIDARG;

    WMTHeapInfo info;
    info.options = {}; // FIXME: ensure this agrees with {Buffer|Texture}AllocationFlag?
    info.size = size_aligned;
    info.sparse_page_size = WMTSparsePageSize16;
    info.type = WMTHeapTypePlacement;

    heap = device_->GetMTLDevice().newHeap(info);
    if (!heap)
      return E_OUTOFMEMORY;

    device_->RegisterResidency(heap);

    return S_OK;
  }

#ifdef DXMT_NATIVE
  HRESULT
  InitializeExternal(int fd, uint64_t offset, uint64_t size, D3D12_HEAP_TYPE type, D3D12_HEAP_FLAGS flags) {
    const uint64_t page = (uint64_t)getpagesize();
    if (fd < 0 || !size || size > (uint64_t(1) << 32) || ((offset | size) & (page - 1)))
      return E_INVALIDARG;

    /* The app's type and flags are advisory: what placed resources are checked against. A custom heap the guest
     * maps is CPU-visible memory on this UMA device. */
    desc_ = {};
    desc_.SizeInBytes = size;
    desc_.Properties.Type = type;
    if (type == D3D12_HEAP_TYPE_CUSTOM) {
      desc_.Properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
      desc_.Properties.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
    }
    desc_.Properties.CreationNodeMask = 1;
    desc_.Properties.VisibleNodeMask = 1;
    desc_.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    desc_.Flags = flags & ~(D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_ALLOW_DISPLAY);

    external_fd = dup(fd);
    if (external_fd < 0)
      return E_OUTOFMEMORY;
    external_offset = offset;
    return S_OK;
  }
#endif

  virtual D3D12_HEAP_DESC *STDMETHODCALLTYPE
  GetDesc(D3D12_HEAP_DESC *__ret) {
    *__ret = desc_;
    return __ret;
  };
};

HRESULT
CreateHeap(MTLD3D12Device *pDevice, const D3D12_HEAP_DESC *pDesc, REFIID riid, void **ppHeap) {
  InitReturnPtr(ppHeap);
  auto heap = Com(new MTLD3D12HeapImpl(pDevice));
  HRESULT hr = heap->Initialize(pDesc);
  if (FAILED(hr))
    return hr;
  if (!ppHeap)
    return S_FALSE;
  return heap->QueryInterface(riid, ppHeap);
}

}; // namespace dxmt

#ifdef DXMT_NATIVE
/* directx-native's DXN_SYM_OPEN_EXISTING_HEAP_FROM_FD: [offset, offset + size) of a shared memory fd (borrowed) as
 * an ID3D12Heap, so buffers placed on it are the embedder's guest's pages: its CPU writes and the GPU see one
 * allocation. Textures placed on it get memory of their own. */
extern "C" int32_t
dxmt_open_existing_heap_from_fd(
    void *device, int fd, uint64_t offset, uint64_t size, uint32_t heap_type, uint32_t heap_flags, const void *iid,
    void **ppHeap
) {
  using namespace dxmt;
  InitReturnPtr(ppHeap);
  if (!device || !iid || !ppHeap)
    return E_INVALIDARG;
  Com<ID3D12Device> d3d12_device;
  if (FAILED(static_cast<IUnknown *>(device)->QueryInterface(IID_PPV_ARGS(&d3d12_device))))
    return E_INVALIDARG;
  auto heap = Com(new MTLD3D12HeapImpl(static_cast<MTLD3D12Device *>(d3d12_device.ptr())));
  HRESULT hr = heap->InitializeExternal(fd, offset, size, (D3D12_HEAP_TYPE)heap_type, (D3D12_HEAP_FLAGS)heap_flags);
  if (FAILED(hr))
    return hr;
  return heap->QueryInterface(*static_cast<const IID *>(iid), ppHeap);
}
#endif