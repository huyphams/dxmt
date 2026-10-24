/*
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

/* Changed for VMPal, 2026-10-24: Direct3D 12 pipelines' AIR in DXMT's shader cache (see CompileAIR). */

#include "d3d12_pipeline.hpp"
#include "dxmt_shader_cache.hpp"
#include "sha1/sha1_util.hpp"
#include "log/log.hpp"

namespace dxmt {

/* Hashes the arguments' contents into `h`, following `next`: false for a kind it doesn't know, whose contents it can't
 * tell apart, so the shader isn't cached. The metal version the shaders are converted for goes to `metal_version`. */
static bool
HashArguments(Sha1HashState &h, SM50_SHADER_COMPILATION_ARGUMENT_DATA *args, WMTMetalVersion &metal_version) {
  for (auto arg = args; arg; arg = (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)arg->next) {
    h.update(arg->type);
    switch (arg->type) {
    case SM50_SHADER_COMMON: {
      auto data = (SM50_SHADER_COMMON_DATA *)arg;
      h.update(data->metal_version);
      h.update(data->flags);
      metal_version = (WMTMetalVersion)data->metal_version;
      break;
    }
    case SM50_SHADER_PSO_PIXEL_SHADER: {
      auto data = (SM50_SHADER_PSO_PIXEL_SHADER_DATA *)arg;
      h.update(data->sample_mask);
      h.update(data->dual_source_blending);
      h.update(data->disable_depth_output);
      h.update(data->unorm_output_reg_mask);
      h.update(data->pixel_formats);
      h.update(data->uint_output_reg_mask);
      h.update(data->sint_output_reg_mask);
      h.update(data->forced_sample_count);
      break;
    }
    case SM50_SHADER_IA_INPUT_LAYOUT: {
      auto data = (SM50_SHADER_IA_INPUT_LAYOUT_DATA *)arg;
      h.update(data->index_buffer_format);
      h.update(data->slot_mask);
      h.update(data->num_elements);
      for (uint32_t i = 0; i < data->num_elements; i++) {
        auto &element = data->elements[i];
        h.update(element.reg);
        h.update(element.slot);
        h.update(element.aligned_byte_offset);
        h.update(element.format);
        h.update((uint32_t)element.step_function);
        h.update((uint32_t)element.step_rate);
      }
      break;
    }
    case SM50_SHADER_ROOT_SIGNATURE: {
      auto data = (SM50_SHADER_ROOT_SIGNATURE_DATA *)arg;
      h.update(data->bytecode_length);
      h.update(data->bytecode, data->bytecode_length);
      break;
    }
    case SM50_SHADER_PSO_GEOMETRY_SHADER: {
      auto data = (SM50_SHADER_PSO_GEOMETRY_SHADER_DATA *)arg;
      h.update(data->strip_topology);
      break;
    }
    case SM50_SHADER_PSO_TESSELLATOR: {
      auto data = (SM50_SHADER_PSO_TESSELLATOR_DATA *)arg;
      h.update(data->max_potential_tess_factor);
      break;
    }
    default:
      return false;
    }
  }
  return true;
}

WMT::Reference<WMT::DispatchData>
CompileAIR(
    std::initializer_list<D3D12_SHADER_BYTECODE> shaders, SM50_SHADER_COMPILATION_ARGUMENT_DATA *args, const char *name,
    const std::function<int(sm50_bitcode_t *, sm50_error_t *)> &compile, SM50Error &error
) {
  Sha1HashState h;
  for (auto &shader : shaders) {
    h.update(shader.BytecodeLength);
    h.update(shader.pShaderBytecode, shader.BytecodeLength);
  }
  auto shaders_digest = h.final();

  Sha1HashState a;
  a.update("d3d12", 5);
  a.update(name, strlen(name));
  auto metal_version = WMTMetal310;
  bool cacheable = HashArguments(a, args, metal_version);
  auto arguments_digest = a.final();
  auto key = std::make_pair(shaders_digest, arguments_digest);

  auto &cache = ShaderCache::getInstance(metal_version);
  if (cacheable) {
    if (auto reader = cache.getReader()) {
      if (auto air = reader->get(key))
        return air;
    }
  }

  SM50ShaderBitcode bitcode;
  if (compile(&bitcode, &error))
    return {};
  SM50_COMPILED_BITCODE compiled;
  SM50GetCompiledBitcode(bitcode, &compiled);
  auto air = WMT::MakeDispatchData(compiled.Data, compiled.Size);
  if (cacheable) {
    if (auto writer = cache.getWriter())
      writer->set(key, air);
  }
  return air;
}

} // namespace dxmt
