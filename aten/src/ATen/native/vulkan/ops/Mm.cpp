#include <ATen/native/vulkan/ops/Mm.h>
#include <ATen/native/vulkan/ops/Utils.h>

#include <ATen/Context.h>
#include <ATen/native/vulkan/api/Tensor.h>
#include <ATen/native/vulkan/api/Types.h>
#include <ATen/native/vulkan/impl/Packing.h>
#include <c10/util/irange.h>

namespace at {
namespace native {
namespace vulkan {
namespace ops {
namespace {

using namespace api::utils;
using namespace at::native::vulkan::ops;

vTensor pack_inputs_using_height_packing(const Tensor& input_arg) {
  TORCH_INTERNAL_ASSERT(
      !input_arg.is_quantized(),
      "Vulkan Linear not usable! "
      "Reason: Input packing only supports non-quantized tensors.");
  TORCH_INTERNAL_ASSERT(
      input_arg.dim() == 2,
      "Vulkan Linear not usable! "
      "Reason: Input packing only supports 2D tensors.");

  Tensor input = input_arg;
  if (input.is_cpu()) {
    input = input.vulkan();
  }

  TORCH_CHECK(input.is_vulkan(), "Input must be on Vulkan device!");

  vTensor v_input = convert(input);
  if (v_input.gpu_memory_layout() ==
      api::GPUMemoryLayout::TENSOR_CHANNELS_PACKED) {
    v_input = packing::convert_image_channels_packed_to_width_packed(v_input);
  }

  TORCH_CHECK(
      v_input.gpu_memory_layout() == api::GPUMemoryLayout::TENSOR_WIDTH_PACKED,
      "After packing, the v_input must be in TENSOR_WIDTH_PACKED format");

  return v_input;
}

vTensor pack_weights_using_height_packing(const Tensor& weight_arg) {
  // Only non-batch, non-quantized tensors are supported
  TORCH_INTERNAL_ASSERT(
      !weight_arg.is_quantized(),
      "Vulkan Linear not usable! "
      "Reason: Weight packing only supports non-quantized tensors.");
  TORCH_INTERNAL_ASSERT(
      weight_arg.dim() == 2,
      "Vulkan Linear not usable! "
      "Reason: Weight packing only supports 2D tensors.");

  Tensor weight = weight_arg;

  if (weight.is_cpu()) {
    weight = weight.vulkan();
  }

  TORCH_CHECK(weight.is_vulkan(), "Weight must be on Vulkan device!");

  vTensor v_weight = convert(weight);
  if (v_weight.gpu_memory_layout() ==
      api::GPUMemoryLayout::TENSOR_CHANNELS_PACKED) {
    v_weight =
        packing::convert_image_channels_packed_to_height_packed(v_weight);
  }

  TORCH_CHECK(
      v_weight.gpu_memory_layout() ==
          api::GPUMemoryLayout::TENSOR_HEIGHT_PACKED,
      "After packing, the v_weight must be in TENSOR_HEIGHT_PACKED format");

  return v_weight;
}

vTensor pack_weights(const Tensor& weight_arg, const bool use_batch = false) {
  if (!weight_arg.is_quantized() && !use_batch) {
    return pack_weights_using_height_packing(weight_arg);
  }

  // Rest of the logic are either quantized or batched.

  bool quantized = false;
  switch (weight_arg.scalar_type()) {
    case at::kQInt8:
    case at::kQUInt8:
      quantized = true;
      break;
    default:
      break;
  }

  api::Context* const context = api::context();

  const Tensor weight = weight_arg.contiguous();
  const IntArrayRef w_sizes = weight.sizes();
  if (use_batch) {
    TORCH_CHECK(
        w_sizes.size() == 3,
        "Vulkan Linear not usable! "
        "Reason: Unable to perform weight packing with batch; the input tensor of a batch of matrices should contain 3 dimensions: batch, height, width.");
  }
  /* Source */
  int64_t src_kb_sz = 0;
  int64_t src_kw_sz = 0;
  int64_t src_kh_sz = 0;
  /* Destination */
  int64_t dst_kb_sz = 0;
  int64_t dst_kw_sz = 0;
  int64_t dst_kh_sz = 0;
  std::vector<int64_t> dst_vtensor_sizes;
  /* Source */
  src_kb_sz = use_batch ? w_sizes[Layout::BatchMatrices::batch] : 1;
  src_kw_sz = use_batch ? w_sizes[Layout::BatchMatrices::width]
                        : w_sizes[Layout::Parameter::width];
  src_kh_sz = use_batch ? w_sizes[Layout::BatchMatrices::height]
                        : w_sizes[Layout::Parameter::height];

  /* Destination */
  dst_kb_sz = src_kb_sz;
  dst_kw_sz = div_up(src_kw_sz, INT64_C(2));
  dst_kh_sz = div_up(src_kh_sz, INT64_C(2));
  dst_vtensor_sizes = {
      dst_kb_sz,
      4,
      dst_kh_sz,
      dst_kw_sz,
  };

  vTensor v_weight{context, dst_vtensor_sizes, weight_arg.scalar_type()};

  if (quantized) {
    v_weight.set_is_quantized();
    v_weight.set_scale(weight_arg.q_scale());
    v_weight.set_zero_point(weight_arg.q_zero_point());
  }

  if (quantized) {
    stage_pack_weights<int8_t>(
        context,
        v_weight,
        weight,
        src_kb_sz,
        src_kh_sz,
        src_kw_sz,
        dst_kh_sz,
        dst_kw_sz);
  } else if (use_batch) {
    stage_pack_weights<float>(
        context,
        v_weight,
        weight,
        src_kb_sz,
        src_kh_sz,
        src_kw_sz,
        dst_kh_sz,
        dst_kw_sz);
  }
  return v_weight;
}

vTensor pack_biases(
    const Tensor& weight_arg,
    const c10::optional<Tensor>& bias_arg,
    const bool use_batch = false) {
  if (bias_arg && bias_arg->is_vulkan()) {
    return convert(*bias_arg);
  }

  api::Context* const context = api::context();

  if (bias_arg) {
    const Tensor bias = bias_arg->contiguous();
    const IntArrayRef b_sizes = bias.sizes();
    const float* const src_bias_ptr = bias.data_ptr<float>();

    /* Source */
    int64_t src_kb_sz = 0;
    int64_t src_kw_sz = 0;
    int64_t src_kh_sz = 0;
    if (use_batch) {
      if (bias.sizes().size() == 3) {
        src_kb_sz = b_sizes[Layout::BatchMatrices::batch];
        src_kw_sz = b_sizes[Layout::BatchMatrices::width];
        src_kh_sz = b_sizes[Layout::BatchMatrices::height];
      } else if (bias.sizes().size() == 2) {
        // skip batch dim for boardcasting; index -1
        src_kb_sz = 1;
        src_kw_sz = b_sizes[Layout::BatchMatrices::height];
        src_kh_sz = b_sizes[Layout::BatchMatrices::batch];
      } else {
        // skip batch & height dim for boardcasting; index -2
        src_kb_sz = 1;
        src_kw_sz = b_sizes[Layout::BatchMatrices::batch];
        src_kh_sz = 1;
      }
    } else if (bias.scalar_type() == at::kFloat) {
      // if it is float/unquantized and not batch case, we use improved
      // algorithm with no packed bias for compiler-optimizable broadcasting
      return convert(bias.vulkan());
      ;
    }
    const int64_t src_matrix_sz = src_kw_sz * src_kh_sz;

    /* Destination */
    const int64_t dst_kw_sz = div_up(src_kw_sz, INT64_C(2));
    const int64_t dst_kh_sz = div_up(src_kh_sz, INT64_C(2));
    const int64_t dst_plane_sz = dst_kw_sz * dst_kh_sz;
    const int64_t dst_matrix_sz = dst_plane_sz * 4;

    vTensor v_bias{
        context,
        {
            src_kb_sz,
            4,
            dst_kh_sz,
            dst_kw_sz,
        },
        bias_arg->scalar_type(),
    };

    api::StorageBuffer staging(context, at::kFloat, v_bias.gpu_numel());
    {
      api::MemoryMap mapping(staging.buffer(), api::MemoryAccessType::WRITE);

      float* dst_bias_ptr = mapping.template data<float>();

      memset(dst_bias_ptr, 0, v_bias.nbytes());

      for (const auto src_b : c10::irange(src_kb_sz)) {
        for (const auto src_h : c10::irange(src_kh_sz == 1 ? 2 : src_kh_sz)) {
          for (const auto src_w :
               c10::irange((use_batch && src_kw_sz == 1) ? 2 : src_kw_sz)) {
            int64_t dst_plane = 2 * (src_h % 2) + (src_w % 2);
            int64_t dst_index = (src_h / 2) * dst_kw_sz + (src_w / 2);
            memcpy(
                dst_bias_ptr + src_b * dst_matrix_sz +
                    dst_plane * dst_plane_sz + dst_index,
                src_bias_ptr + src_b * src_matrix_sz +
                    (src_kh_sz == 1 ? 0 : src_h * src_kw_sz) +
                    ((use_batch && src_kw_sz == 1) ? 0 : src_w),
                sizeof(float));
          }
        }
      }
    }
    utils::pack_staging_to_vtensor(staging.buffer(), v_bias);

    return v_bias;
  } else {
    vTensor v_bias{
        api::context(),
        {1},
        weight_arg.scalar_type(),
    };

    api::StorageBuffer staging(context, at::kFloat, v_bias.gpu_numel());
    {
      api::MemoryMap mapping(staging.buffer(), api::MemoryAccessType::WRITE);

      float* data_ptr = mapping.template data<float>();

      memset(
          data_ptr,
          // 2's complement integers and IEEE-754 floating point numbers both
          // have identical bit representations for 0, so can use memset which
          // only accepts uint8_t parameter.
          0,
          v_bias.nbytes());
    }
    utils::pack_staging_to_vtensor(staging.buffer(), v_bias);

    return v_bias;
  }
}

bool available_check_with_batch(
    const Tensor& weight,
    const c10::optional<Tensor>& bias) {
  const bool weight_available = (3 == weight.ndimension()) &&
      (weight.size(Layout::BatchMatrices::batch) > 0) &&
      (weight.size(Layout::BatchMatrices::height) > 0) &&
      (weight.size(Layout::BatchMatrices::width) > 0) &&
      ((weight.device().is_cpu()) ||
       (c10::DeviceType::Vulkan == weight.device().type())) &&
      (kFloat == weight.scalar_type()) && !weight.requires_grad();
  if (!weight_available) {
    return false;
  }

  if (!bias || !bias->defined()) {
    // no need to check bias since it is not used.
    return true;
  }

  bool bias_available = true;
  bias_available &= (bias->ndimension() > 0);
  bias_available &=
      ((bias->device().is_cpu()) ||
       (c10::DeviceType::Vulkan == bias->device().type()));
  bias_available &= (kFloat == bias->scalar_type());
  // Only check the consistency of batch and width dimension. The height
  // dimension consistency is unchecked, due to the 2nd input which determines
  // the height is not passed into LinearPackedContext.
  if (bias->ndimension() == 3) {
    bias_available &=
        (bias->size(Layout::BatchMatrices::width) ==
             weight.size(Layout::BatchMatrices::width) ||
         bias->size(Layout::BatchMatrices::width) == 1);
    bias_available &=
        (bias->size(Layout::BatchMatrices::batch) ==
             weight.size(Layout::BatchMatrices::batch) ||
         bias->size(Layout::BatchMatrices::batch) == 1);
  } else if (bias->ndimension() == 2) {
    // skip batch dim for boardcasting; index -1
    bias_available &=
        (bias->size(Layout::BatchMatrices::height) ==
             weight.size(Layout::BatchMatrices::width) ||
         bias->size(Layout::BatchMatrices::height) == 1);
  } else {
    // skip batch & height dim for boardcasting; index -2
    bias_available &=
        (bias->size(Layout::BatchMatrices::batch) ==
             weight.size(Layout::BatchMatrices::width) ||
         bias->size(Layout::BatchMatrices::batch) == 1);
  }
  bias_available &= !bias->requires_grad();
  return bias_available;
}

bool available(
    const Tensor& weight,
    const c10::optional<Tensor>& bias,
    const bool use_batch = false) {
  if (!api::available()) {
    return false;
  }

  if (use_batch) {
    return available_check_with_batch(weight, bias);
  }

  const bool weight_available = (2 == weight.ndimension()) &&
      (weight.size(Layout::Parameter::height) > 0) &&
      (weight.size(Layout::Parameter::width) > 0) &&
      ((weight.device().is_cpu()) ||
       (c10::DeviceType::Vulkan == weight.device().type())) &&
      (kFloat == weight.scalar_type() || kQInt8 == weight.scalar_type()) &&
      !weight.requires_grad();
  if (!weight_available) {
    return false;
  }

  const bool bias_available =
      ((bias && bias.has_value() && bias->defined())
           ? ((bias->ndimension() > 0) &&
              ((bias->device().is_cpu()) ||
               (c10::DeviceType::Vulkan == bias->device().type())) &&
              (kFloat == bias->scalar_type()) &&
              ((bias->ndimension() > 1)
                   ? (bias->size(Layout::Parameter::width) ==
                      weight.size(Layout::Parameter::width))
                   : true) &&
              !bias->requires_grad())
           : true);
  return bias_available;
}

bool usable_check_with_batch(
    const Tensor& input,
    const IntArrayRef unpacked_weight_sizes) {
  return (3 == input.ndimension()) &&
      (c10::DeviceType::Vulkan == input.device().type()) &&
      (kFloat == input.scalar_type()) &&
      (input.size(Layout::BatchMatrices::width) ==
       unpacked_weight_sizes[Layout::BatchMatrices::height]) &&
      (input.size(Layout::BatchMatrices::batch) ==
       unpacked_weight_sizes[Layout::BatchMatrices::batch]) &&
      !input.requires_grad() && true;
}

bool usable(
    const Tensor& input,
    const IntArrayRef unpacked_weight_sizes,
    const bool use_batch = false) {
  if (use_batch) {
    return usable_check_with_batch(input, unpacked_weight_sizes);
  }
  const auto v_input = convert(input);
  return (2 == input.ndimension()) &&
      (c10::DeviceType::Vulkan == input.device().type()) &&
      ((kFloat == input.scalar_type()) ||
       (v_input.is_quantized() &&
        (kQUInt8 == input.scalar_type() || kQInt8 == input.scalar_type()))) &&
      (input.size(Layout::Parameter::width) ==
       unpacked_weight_sizes[Layout::Parameter::height]) &&
      !input.requires_grad() && true;
}

static Tensor reshape_to_2d(const Tensor& input_arg) {
  TORCH_CHECK(
      input_arg.dim() >= 2,
      "Vulkan Linear op only supports input tensor with dim >= 2");
  const IntArrayRef input_sizes = input_arg.sizes();
  const auto d =
      c10::multiply_integers(input_sizes.cbegin(), input_sizes.end() - 1);
  return input_arg.reshape({d, input_arg.size(-1)});
}

Tensor run_addmm_context(
    const Tensor& input_arg,
    const float alpha,
    const float beta,
    const c10::intrusive_ptr<LinearPackedContext>& linear_context,
    bool quantized,
    double output_scale,
    int64_t output_zero_point) {
  api::Context* const context = api::context();

  const Tensor input_arg_2d =
      input_arg.dim() == 2 ? input_arg : reshape_to_2d(input_arg);
  const Tensor input =
      input_arg_2d.is_vulkan() ? input_arg_2d : input_arg_2d.vulkan();
  const vTensor& v_input = pack_inputs_using_height_packing(input);

  const vTensor& packed_v_weight = convert(
      linear_context->get_val(LinearPackedContext::Packed::Weight).toTensor());
  const vTensor& packed_v_bias = convert(
      linear_context->get_val(LinearPackedContext::Packed::Bias).toTensor());
  const std::vector<int64_t> unpacked_weight_sizes =
      linear_context->get_val(LinearPackedContext::Packed::WeightSizes)
          .toIntVector();
  const bool bias_defined =
      linear_context->get_val(LinearPackedContext::Packed::BiasDefined)
          .toBool();

  TORCH_CHECK(
      usable(input, unpacked_weight_sizes),
      "Vulkan Linear not usable! "
      "Reason: The provided input tensor is either invalid on its own, or its "
      "combination with the provided weight and bias tensors are unsupported by "
      "Vulkan impl.");

  TORCH_CHECK(
      (!quantized ||
       (packed_v_weight.is_quantized() && v_input.is_quantized())),
      "run_addmm_context called for quantized version with unquantized input");

  TORCH_CHECK(
      (quantized ||
       packed_v_weight.gpu_memory_layout() ==
           api::GPUMemoryLayout::TENSOR_HEIGHT_PACKED),
      "run_addmm_context called for non-quantized version with unpacked weight");

  vTensor v_output{
      context,
      {
          input_arg_2d.sizes()[Layout::Parameter::height],
          unpacked_weight_sizes[Layout::Parameter::width],
      },
      input_arg.scalar_type(),
  };

  if (quantized) {
    v_output.set_is_quantized();
    v_output.set_scale(output_scale);
    v_output.set_zero_point(output_zero_point);
  }

  if (bias_defined) {
    api::UniformParamsBuffer params;
    api::ShaderInfo compute_shader;

    if (quantized) {
      compute_shader = (kQInt8 == input_arg.scalar_type())
          ? VK_KERNEL(quantized_addmm_qint8)
          : VK_KERNEL(quantized_addmm_quint8);
      const struct {
        uvec3 size;
        int32_t K;
        uvec3 um1_size;
        int32_t K1;
        uvec3 um2_size;
        int32_t K2;
        uvec3 ut_size;
        int32_t K3;
        vec2 multiplier;
        vec2 input_scales;
        float out_scale;
        float _1;
        ivec2 input_zero_points;
        int32_t out_zero_point;
        int32_t _2;
      } block{
          v_output.extents(),
          safe_downcast<int32_t>(
              div_up(v_input.sizes()[Layout::Parameter::width], INT64_C(2))),
          v_input.extents(),
          0u,
          packed_v_weight.extents(),
          0u,
          packed_v_bias.extents(),
          0u,
          {
              alpha,
              beta,
          },
          {
              safe_downcast<float>(v_input.get_scale()),
              safe_downcast<float>(packed_v_weight.get_scale()),
          },
          safe_downcast<float>(output_scale),
          0.0f,
          {
              safe_downcast<int32_t>(v_input.get_zero_point()),
              safe_downcast<int32_t>(packed_v_weight.get_zero_point()),
          },
          safe_downcast<int32_t>(output_zero_point),
          0u,
      };
      params = api::UniformParamsBuffer(context, block);
    } else {
      compute_shader = VK_KERNEL(addmm);
      const struct {
        uvec3 shader_extents;
        uint32_t tensor_row_size;
        uvec3 bias_size;
        int32_t _;
        vec2 multiplier;
      } block{
          v_output.extents(),
          v_input.extents().data[0u],
          packed_v_bias.extents(),
          0,
          {
              alpha,
              beta,
          },
      };

      params = api::UniformParamsBuffer(context, block);
    }

    api::PipelineBarrier pipeline_barrier{};
    context->submit_compute_job(
        // shader descriptor
        compute_shader,
        // pipeline barrier
        pipeline_barrier,
        // global work group size
        {
            safe_downcast<uint32_t>(
                div_up(v_output.sizes()[Layout::Parameter::width], INT64_C(4))),
            safe_downcast<uint32_t>(div_up(
                v_output.sizes()[Layout::Parameter::height], INT64_C(4))),
            1,
        },
        // local work group size
        {8, 8, 1},
        // fence handle
        VK_NULL_HANDLE,
        // shader arguments
        v_output.image(
            pipeline_barrier,
            api::PipelineStage::COMPUTE,
            api::MemoryAccessType::WRITE),
        v_input.image(pipeline_barrier, api::PipelineStage::COMPUTE),
        packed_v_weight.image(pipeline_barrier, api::PipelineStage::COMPUTE),
        packed_v_bias.image(pipeline_barrier, api::PipelineStage::COMPUTE),
        // params buffer
        params.buffer());
  } else {
    api::UniformParamsBuffer params;
    api::ShaderInfo compute_shader;
    if (quantized) {
      const struct {
        uvec3 size;
        int32_t K;
        uvec3 um1_size;
        int32_t K1;
        uvec3 um2_size;
        int32_t K2;
        vec2 input_scales;
        float out_scale;
        float _1;
        ivec2 input_zero_points;
        int32_t out_zero_point;
        int32_t _2;
      } block_no_bias{
          v_output.extents(),
          safe_downcast<int32_t>(
              div_up(v_input.sizes()[Layout::Parameter::width], INT64_C(2))),
          v_input.extents(),
          0u,
          packed_v_weight.extents(),
          0u,
          {
              safe_downcast<float>(v_input.get_scale()),
              safe_downcast<float>(packed_v_weight.get_scale()),
          },
          safe_downcast<float>(output_scale),
          0.0f,
          {
              safe_downcast<int32_t>(v_input.get_zero_point()),
              safe_downcast<int32_t>(packed_v_weight.get_zero_point()),
          },
          safe_downcast<int32_t>(output_zero_point),
          0u,
      };
      params = api::UniformParamsBuffer(context, block_no_bias);
      compute_shader = (kQInt8 == input_arg.scalar_type())
          ? VK_KERNEL(quantized_mm_qint8)
          : VK_KERNEL(quantized_mm_quint8);
    } else {
      const struct {
        uvec3 shader_extents;
        uint32_t tensor_row_size;
      } block_no_bias{
          v_output.extents(),
          v_input.extents().data[0u],
      };
      params = api::UniformParamsBuffer(context, block_no_bias);
      compute_shader = VK_KERNEL(mm);
    }

    api::PipelineBarrier pipeline_barrier{};

    context->submit_compute_job(
        // shader descriptor
        compute_shader,
        // pipeline barrier
        pipeline_barrier,
        // global work group size
        {
            safe_downcast<uint32_t>(
                div_up(v_output.sizes()[Layout::Parameter::width], INT64_C(4))),
            safe_downcast<uint32_t>(div_up(
                v_output.sizes()[Layout::Parameter::height], INT64_C(4))),
            1,
        },
        // local work group size
        {8, 8, 1},
        // fence handle
        VK_NULL_HANDLE,
        // shader arguments
        v_output.image(
            pipeline_barrier,
            api::PipelineStage::COMPUTE,
            api::MemoryAccessType::WRITE),
        v_input.image(pipeline_barrier, api::PipelineStage::COMPUTE),
        packed_v_weight.image(pipeline_barrier, api::PipelineStage::COMPUTE),
        // params buffer
        params.buffer());
  }

  Tensor output = convert(v_output);
  if (input_arg.dim() == 2) {
    return output;
  } else {
    std::vector<int64_t> shape;
    for (const auto i : c10::irange(input_arg.dim() - 1)) {
      shape.emplace_back(input_arg.size(i));
    }
    shape.emplace_back(output.size(-1));
    return output.reshape(shape);
  }
}

Tensor run_baddbmm_context(
    const Tensor& input_arg,
    const float alpha,
    const float beta,
    const c10::intrusive_ptr<LinearPackedContext>& linear_context) {
  api::Context* const context = api::context();

  TORCH_CHECK(
      input_arg.dim() == 3,
      "Vulkan Linear not usable! "
      "Reason: The input has the wrong dimension; the tensor of a batch of matrices should contain 3 dimensions: batch, height, width.");

  const Tensor input = input_arg.is_vulkan() ? input_arg : input_arg.vulkan();
  const vTensor& v_input = convert(input);

  const vTensor& packed_v_weight = convert(
      linear_context->get_val(LinearPackedContext::Packed::Weight).toTensor());
  const vTensor& packed_v_bias = convert(
      linear_context->get_val(LinearPackedContext::Packed::Bias).toTensor());
  const std::vector<int64_t> unpacked_weight_sizes =
      linear_context->get_val(LinearPackedContext::Packed::WeightSizes)
          .toIntVector();
  const bool bias_defined =
      linear_context->get_val(LinearPackedContext::Packed::BiasDefined)
          .toBool();

  TORCH_CHECK(
      usable(input, unpacked_weight_sizes, true /*use batch*/),
      "Vulkan Linear not usable! "
      "Reason: The provided input tensor is either invalid on its own, or its "
      "combination with the provided weight and bias tensors are unsupported by "
      "Vulkan impl.");

  vTensor v_output{
      context,
      {
          v_input.sizes()[Layout::BatchMatrices::batch],
          v_input.sizes()[Layout::BatchMatrices::height],
          unpacked_weight_sizes[Layout::BatchMatrices::width],
      },
      input_arg.scalar_type(),
  };

  if (bias_defined) {
    const struct {
      uvec3 size;
      int32_t k;
      uvec3 bias_size;
      int32_t _;
      vec2 multiplier;
    } block{
        v_output.extents(),
        safe_downcast<int32_t>(
            div_up(v_input.sizes()[Layout::BatchMatrices::width], INT64_C(2))),
        packed_v_bias.extents(),
        0,
        {
            alpha,
            beta,
        },
    };

    api::UniformParamsBuffer params(context, block);
    api::PipelineBarrier pipeline_barrier{};

    context->submit_compute_job(
        // shader descriptor
        VK_KERNEL(baddbmm),
        // pipeline barrier
        pipeline_barrier,
        // global work group size
        {
            safe_downcast<uint32_t>(div_up(
                unpacked_weight_sizes[Layout::BatchMatrices::width],
                INT64_C(2))),
            safe_downcast<uint32_t>(div_up(
                v_input.sizes()[Layout::BatchMatrices::height], INT64_C(2))),
            safe_downcast<uint32_t>(div_up(
                v_input.sizes()[Layout::BatchMatrices::batch], INT64_C(4))),
        },
        // local work group size
        {8, 8, 1},
        // fence handle
        VK_NULL_HANDLE,
        // shader arguments
        v_output.image(
            pipeline_barrier,
            api::PipelineStage::COMPUTE,
            api::MemoryAccessType::WRITE),
        v_input.image(pipeline_barrier, api::PipelineStage::COMPUTE),
        packed_v_weight.image(pipeline_barrier, api::PipelineStage::COMPUTE),
        packed_v_bias.image(pipeline_barrier, api::PipelineStage::COMPUTE),
        // params buffer
        params.buffer());
  } else {
    const struct {
      uvec3 size;
      int32_t k;
    } block_no_bias{
        v_output.extents(),
        safe_downcast<int32_t>(
            div_up(v_input.sizes()[Layout::BatchMatrices::width], INT64_C(2))),
    };

    api::UniformParamsBuffer params(context, block_no_bias);
    api::PipelineBarrier pipeline_barrier{};

    context->submit_compute_job(
        // shader descriptor
        VK_KERNEL(bmm),
        // pipeline barrier
        pipeline_barrier,
        // global work group size
        {
            safe_downcast<uint32_t>(div_up(
                unpacked_weight_sizes[Layout::BatchMatrices::width],
                INT64_C(2))),
            safe_downcast<uint32_t>(div_up(
                v_input.sizes()[Layout::BatchMatrices::height], INT64_C(2))),
            safe_downcast<uint32_t>(div_up(
                v_input.sizes()[Layout::BatchMatrices::batch], INT64_C(4))),
        },
        // local work group size
        {8, 8, 1},
        // fence handle
        VK_NULL_HANDLE,
        // shader arguments
        v_output.image(
            pipeline_barrier,
            api::PipelineStage::COMPUTE,
            api::MemoryAccessType::WRITE),
        v_input.image(pipeline_barrier, api::PipelineStage::COMPUTE),
        packed_v_weight.image(pipeline_barrier, api::PipelineStage::COMPUTE),
        // params buffer
        params.buffer());
  }

  Tensor output = convert(v_output);
  std::vector<int64_t> shape;
  for (const auto i : c10::irange(input_arg.dim() - 1)) {
    shape.emplace_back(input_arg.size(i));
  }
  shape.emplace_back(output.size(-1));
  return output.reshape(shape);
}

Tensor addmm(
    const Tensor& bias,
    const Tensor& input,
    const Tensor& weight,
    const Scalar& beta,
    const Scalar& alpha) {
  return run_addmm_context(
      input,
      alpha.to<float>(),
      beta.to<float>(),
      c10::make_intrusive<LinearPackedContext>(
          LinearPackedContext(weight, bias)),
      false,
      0,
      0);
}

Tensor mm(const Tensor& mat1_arg, const Tensor& mat2_arg) {
  return run_addmm_context(
      mat1_arg,
      1.0f,
      1.0f,
      c10::make_intrusive<LinearPackedContext>(
          LinearPackedContext(mat2_arg, c10::optional<Tensor>())),
      false,
      0,
      0);
}

Tensor bmm(const Tensor& mat1_arg, const Tensor& mat2_arg) {
  return run_baddbmm_context(
      mat1_arg,
      1.0f,
      1.0f,
      c10::make_intrusive<LinearPackedContext>(LinearPackedContext(
          mat2_arg, c10::optional<Tensor>(), true /*use batch*/)));
}

Tensor baddbmm(
    const Tensor& bias,
    const Tensor& input,
    const Tensor& weight,
    const Scalar& beta,
    const Scalar& alpha) {
  return run_baddbmm_context(
      input,
      alpha.to<float>(),
      beta.to<float>(),
      c10::make_intrusive<LinearPackedContext>(
          LinearPackedContext(weight, bias, true /*use batch*/)));
}

#ifdef USE_VULKAN_API

TORCH_LIBRARY_IMPL(aten, Vulkan, m) {
  m.impl(TORCH_SELECTIVE_NAME("aten::addmm"), TORCH_FN(addmm));
  m.impl(TORCH_SELECTIVE_NAME("aten::mm"), TORCH_FN(mm));
  m.impl(TORCH_SELECTIVE_NAME("aten::bmm"), TORCH_FN(bmm));
  m.impl(TORCH_SELECTIVE_NAME("aten::baddbmm"), TORCH_FN(baddbmm));
}

#endif /* USE_VULKAN_API */

} // namespace

LinearPackedContext::LinearPackedContext(
    const Tensor& weight,
    const c10::optional<Tensor>& bias,
    const bool use_batch)
    : unpacked_{c10::AnyType::get()} {
  TORCH_CHECK(
      available(weight, bias, use_batch),
      "Vulkan Linear not available! "
      "Reason: The provided (weight, bias) parameters are either invalid "
      "individually or their combination is not supported by Vulkan Impl.");

  packed_.reserve(Packed::NumArgs);
  packed_.emplace_back(convert(pack_weights(weight, use_batch)));
  packed_.emplace_back(convert(pack_biases(weight, bias, use_batch)));
  packed_.emplace_back(weight.sizes());
  packed_.emplace_back(bias && bias->defined());

  if (!at::globalContext().releaseWeightsWhenPrepacking()) {
    unpacked_.reserve(Unpacked::NumArgs);
    unpacked_.emplace_back(weight);
    unpacked_.emplace_back(bias);
  }
}

LinearPackedContext LinearPackedContext::pack(c10::impl::GenericList unpacked) {
  return LinearPackedContext(
      unpacked.get(Unpacked::Weight).toTensor(),
      get_optional_tensor(unpacked, Unpacked::Bias));
}

c10::intrusive_ptr<LinearPackedContext> create_linear_context(
    Tensor&& weight,
    c10::optional<Tensor>&& bias) {
  return c10::make_intrusive<LinearPackedContext>(
      LinearPackedContext(weight, bias));
}

Tensor run_linear_context(
    const Tensor& input,
    const c10::intrusive_ptr<LinearPackedContext>& linear_context) {
  return run_addmm_context(input, 1.0f, 1.0f, linear_context, false, 0, 0);
}

Tensor run_qlinear_context(
    const Tensor& input_arg,
    double output_scale,
    int64_t output_zero_point,
    const c10::intrusive_ptr<LinearPackedContext>& linear_context) {
  return run_addmm_context(
      input_arg,
      1.0f,
      1.0f,
      linear_context,
      true,
      output_scale,
      output_zero_point);
}

} // namespace ops
} // namespace vulkan
} // namespace native
} // namespace at
