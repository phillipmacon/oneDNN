/*******************************************************************************
* Copyright 2021-2022 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <utility>
#include <vector>
#include <unordered_set>

#include "interface/shape_infer.hpp"

namespace dnnl {
namespace graph {
namespace impl {

/// convert shape to ncx or oix
dims canonicalize(const dims &shape, const std::string &format) {
    dims ret(shape);
    const size_t ndims = shape.size();

    if (ndims <= 2 || "NCX" == format || "OIX" == format) return ret;

    if ("NXC" == format) {
        // NXC -> NCX
        ret[1] = shape[ndims - 1]; // c
        for (size_t i = 2; i < ndims; ++i) {
            ret[i] = shape[i - 1];
        }
    } else if ("XIO" == format) {
        // XIO -> OIX
        ret[0] = shape[ndims - 1]; // oc
        ret[1] = shape[ndims - 2]; // ic
        for (size_t i = 2; i < ndims; ++i) {
            ret[i] = shape[i - 2];
        }
    } else {
        assert(!"invalid format");
    }
    return ret;
}

inline dims ncx2nxc(const dims &shape) {
    const size_t ndims = shape.size();
    if (ndims <= 2) return shape;

    dims ret(shape);
    // x
    for (size_t i = 2; i < ndims; ++i) {
        ret[i - 1] = shape[i];
    }
    // c
    ret[ndims - 1] = shape[1];

    return ret;
}

/// make a dims according to the format. Only for data format ncx or nxc.
inline dims make_data_dims(const std::string &format, const dim_t n,
        const dim_t c, const dims &x) {
    dims ret;
    if (format == "NCX") {
        ret.push_back(n);
        ret.push_back(c);
        ret.insert(ret.end(), x.begin(), x.end());
    } else if (format == "NXC") {
        ret.push_back(n);
        ret.insert(ret.end(), x.begin(), x.end());
        ret.push_back(c);
    } else {
        assert(!"invalid format");
    }

    return ret;
}

/// make a dims according to the format. Only for filter format xio or oix.
inline dims make_filter_dims(const std::string &format, const dim_t i,
        const dim_t o, const dims &x) {
    dims ret;
    if (format == "XIO") {
        ret.insert(ret.begin(), x.begin(), x.end());
        ret.push_back(i);
        ret.push_back(o);
    } else if (format == "OIX") {
        ret.push_back(o);
        ret.push_back(i);
        ret.insert(ret.end(), x.begin(), x.end());
    } else {
        assert(!"invalid format");
    }

    return ret;
}

/// validate the inferred shape with the expected one.
bool validate(const dims &inferred, const dims &expected) {
    if (inferred.size() != expected.size()) { return false; }

    for (size_t i = 0; i < inferred.size(); ++i) {
        if (expected[i] != -1 && inferred[i] != expected[i]) { return false; }
    }
    return true;
}

/// get the dense strides of a given shape
/// eg. (3, 4, 5) -> (20, 5, 1)
inline dims get_dense_strides(const dims &shape) {
    dims strides(shape.size());
    for (auto it = shape.begin(); it < shape.end(); ++it) {
        const auto val = std::accumulate(
                std::next(it), shape.end(), 1, std::multiplies<dim_t>());
        const auto dist = std::distance(shape.begin(), it);
        strides[static_cast<size_t>(dist)] = val;
    }
    return strides;
}

/// shapes of the logical tensors in the vector are known
inline bool every_shape_is_known(const std::vector<logical_tensor_t *> &lts) {
    bool ret = std::all_of(
            lts.cbegin(), lts.cend(), [](const logical_tensor_t *const lt) {
                return !logical_tensor_wrapper_t(lt).is_shape_unknown();
            });
    return ret;
}

inline bool verify_shapes_in_range(const std::vector<logical_tensor_t *> &lts,
        const size_t begin, const size_t end,
        const std::function<bool(const dims)> &validator) {
    for (size_t idx = begin; idx < end; ++idx) {
        const dims ltx_dims = logical_tensor_wrapper_t(lts[idx]).vdims();
        if (!validator(ltx_dims)) return false;
    }

    return true;
}

void set_shape_and_strides(logical_tensor_t &lt, const dims &shape) {
    utils::array_copy(lt.dims, shape.data(), shape.size());
    lt.ndims = static_cast<int32_t>(shape.size());

    auto ltw = logical_tensor_wrapper_t(lt);
    // don't overwrite strides provided by users
    if (ltw.is_strided() && ltw.is_stride_unknown()) {
        const dims strides = get_dense_strides(shape);
        utils::array_copy(lt.layout.strides, strides.data(), strides.size());
    }
}

inline void set_shapes_in_range(const std::vector<logical_tensor_t *> &lts,
        const size_t begin, const size_t end, const dims &shape) {
    for (auto idx = begin; idx < end; ++idx) {
        set_shape_and_strides(*lts[idx], shape);
    }
}

/// infer the padding sizes according auto_pad type
status_t infer_auto_pad(const dim_t in_dim, const dim_t stride,
        const dim_t kernel, const dim_t dilation, const std::string &auto_pad,
        dim_t &pad_begin, dim_t &pad_end, bool is_deconv) {
    if (auto_pad == "VALID") {
        pad_begin = 0;
        pad_end = 0;
    } else if (auto_pad == "SAME_UPPER" || auto_pad == "SAME_LOWER") {
        // TODO(xxx): need to improve?
        if (1 != dilation) return status::unsupported;

        dim_t legacy = (in_dim + stride - 1) / stride;
        dim_t pad_needed = is_deconv ? kernel - stride
                                     : (legacy - 1) * stride + kernel - in_dim;
        pad_begin = auto_pad == "SAME_LOWER" ? ((pad_needed + 1) / 2)
                                             : (pad_needed / 2);
        pad_end = pad_needed - pad_begin;
    } else {
        return status::invalid_argument;
    }

    return status::success;
}

/// numpy broadcasting
/// TODO(xxx): 0-D broadcasting?
status_t broadcast(const dims &lhs, const dims &rhs, dims &broadcasted) {
    const size_t lhs_rank = lhs.size();
    const size_t rhs_rank = rhs.size();
    const size_t max_rank = std::max(lhs_rank, rhs_rank);

    broadcasted.resize(max_rank);
    const size_t bl = max_rank - lhs_rank;
    const size_t br = max_rank - rhs_rank;

    for (size_t index = 0; index < max_rank; ++index) {
        dim_t l = 1, r = 1;
        if (index >= bl) l = lhs[index - bl];
        if (index >= br) r = rhs[index - br];
        if (l != r) {
            if (l != 1 && r != 1) return status::invalid_shape;
            broadcasted[index] = (l == 1 ? r : l);
        } else {
            broadcasted[index] = l;
        }
    }

    return status::success;
}

/// This function assumes the size of all vectors are correct. Eg. size of
/// strides/dilations/pads should be the same as spatial size of src_dims and
/// fil_dims. Size of output_dims should be the same as size of src_dims.
inline void infer_conv_ncx_oix(const dims &src_dims, const dims &fil_dims,
        const dims &strides, const dims &dilations, const dims &pads_begin,
        const dims &pads_end, dims &output_dims) {
    output_dims[0] = src_dims[0]; // n
    output_dims[1] = fil_dims[0]; // c
    for (size_t i = 2; i < src_dims.size(); ++i) {
        dim_t padded = src_dims[i] + pads_begin[i - 2] + pads_end[i - 2];
        dim_t dilated = dilations[i - 2] * (fil_dims[i] - 1) + 1;
        output_dims[i] = ((padded - dilated) / strides[i - 2]) + 1;
    }
}

status_t infer_conv_output_shape(op_t *n,
        std::vector<logical_tensor_t *> &inputs,
        std::vector<logical_tensor_t *> &outputs) {
    auto in0 = logical_tensor_wrapper_t(inputs[0]); // src
    auto in1 = logical_tensor_wrapper_t(inputs[1]); // filter
    auto out0 = logical_tensor_wrapper_t(outputs[0]); // output

    // get attr value
    const dim_t g = n->get_attr<dim_t>("groups");
    const auto &strides = n->get_attr<dims>("strides");
    const auto &dilations = n->get_attr<dims>("dilations");
    const auto &pads_begin = n->get_attr<dims>("pads_begin");
    const auto &pads_end = n->get_attr<dims>("pads_end");
    std::string fil_fmt = n->get_attr<std::string>("filter_format");
    std::string src_fmt = n->get_attr<std::string>("data_format");

    // check if src channel / groups == weight input channel
    if (in0.get_src_c(src_fmt) / g != in1.get_weight_i(fil_fmt)) {
        return status::invalid_shape;
    }

    // spatial dims
    dims src_sp = in0.get_src_spatial_dims(src_fmt);
    dims fil_sp = in1.get_weight_spatial_dims(fil_fmt);

    // if paddings are empty vectors?
    dims new_pads_begin(pads_begin);
    if (new_pads_begin.empty()) { new_pads_begin.assign(src_sp.size(), 0); }
    dims new_pads_end(pads_end);
    if (new_pads_end.empty()) { new_pads_end.assign(src_sp.size(), 0); }

    // strides and dilations are required and should be correctly provided.
    if (strides.size() != src_sp.size() || dilations.size() != fil_sp.size()
            || new_pads_begin.size() != src_sp.size()
            || new_pads_end.size() != src_sp.size()) {
        return status::invalid_shape;
    }

    if (n->has_attr("auto_pad")
            && n->get_attr<std::string>("auto_pad") != "None") {
        std::string auto_pad = n->get_attr<std::string>("auto_pad");
        // infer auto padding sizes
        for (size_t i = 0; i < src_sp.size(); ++i) {
            infer_auto_pad(src_sp[i], strides[i], fil_sp[i], dilations[i],
                    auto_pad, new_pads_begin[i], new_pads_end[i]);
        }

        n->set_attr("pads_begin", new_pads_begin);
        n->set_attr("pads_end", new_pads_end);
    }

    // infer output shape
    dims output_dims(in0.vdims());
    infer_conv_ncx_oix(canonicalize(in0.vdims(), src_fmt),
            canonicalize(in1.vdims(), fil_fmt), strides, dilations,
            new_pads_begin, new_pads_end, output_dims);
    // output shape should have the same format as input data
    if ("NXC" == src_fmt) { output_dims = ncx2nxc(output_dims); }

    if (out0.ndims() != -1) {
        if (!validate(output_dims, out0.vdims())) {
            return status::invalid_shape;
        }
    }

    set_shape_and_strides(*outputs[0], output_dims);
    return status::success;
}

status_t infer_conv_bprop_data_output_shape(op_t *n,
        std::vector<logical_tensor_t *> &inputs,
        std::vector<logical_tensor_t *> &outputs) {
    auto in0 = logical_tensor_wrapper_t(inputs[0]); // data
    auto in1 = logical_tensor_wrapper_t(inputs[1]); // filter

    // get attr value
    const dim_t g = n->get_attr<dim_t>("groups");
    const auto &strides = n->get_attr<dims>("strides");
    const auto &dilations = n->get_attr<dims>("dilations");
    const auto &pads_begin = n->get_attr<dims>("pads_begin");
    const auto &pads_end = n->get_attr<dims>("pads_end");
    std::string fil_fmt = n->get_attr<std::string>("filter_format");
    std::string src_fmt = n->get_attr<std::string>("data_format");

    // check if diff_dst channel == weight output channel.
    // Since the input of conv_bwd_data op is diff_dst, which has the same shape
    // with conv fwd op's dst, so it's channel should be equal to weight's o
    // channel.
    if (in0.get_src_c(src_fmt) != in1.get_weight_o(fil_fmt)) {
        return status::invalid_shape;
    }

    dims src_sp = in0.get_src_spatial_dims(src_fmt);
    dims fil_sp = in1.get_weight_spatial_dims(fil_fmt);

    // if paddings are empty vectors?
    dims new_pads_begin(pads_begin);
    if (new_pads_begin.empty()) { new_pads_begin.assign(src_sp.size(), 0); }
    dims new_pads_end(pads_end);
    if (new_pads_end.empty()) { new_pads_end.assign(src_sp.size(), 0); }

    // strides and dilations are required and should be correctly provided.
    if (strides.size() != src_sp.size() || dilations.size() != fil_sp.size()
            || new_pads_begin.size() != src_sp.size()
            || new_pads_end.size() != src_sp.size()) {
        return status::invalid_shape;
    }

    dims output_padding(src_sp.size(), 0);
    if (n->has_attr("output_padding")) {
        output_padding = n->get_attr<dims>("output_padding");
    }

    if (n->has_attr("auto_pad")
            && n->get_attr<std::string>("auto_pad") != "None") {
        std::string auto_pad = n->get_attr<std::string>("auto_pad");

        // infer auto_pad
        for (size_t i = 0; i < src_sp.size(); ++i) {
            infer_auto_pad(src_sp[i], strides[i], fil_sp[i], dilations[i],
                    auto_pad, new_pads_begin[i], new_pads_end[i]);
        }

        n->set_attr("pads_begin", new_pads_begin);
        n->set_attr("pads_end", new_pads_end);
    }

    dims output_sp;
    // third input - output_shape is optional.
    // When output_shape is specified pads_begin and pads_end are ignored,
    // and auto_pad defines how to distribute padding amount around the tensor.
    if (inputs.size() == 3
            && logical_tensor_wrapper_t(inputs[2]).ndims() != -1) {
        // Since we have no access to the data of the third input
        // (output_shape), we cannot set output spatial shape.
        return status::unsupported;
    } else {
        for (size_t i = 0; i < src_sp.size(); ++i) {
            dim_t padded
                    = output_padding[i] - new_pads_begin[i] - new_pads_end[i];
            dim_t dilated = dilations[i] * (fil_sp[i] - 1) + 1;
            output_sp.push_back(
                    strides[i] * (src_sp[i] - 1) + dilated + padded);
        }
    }

    const dims out0_shape = make_data_dims(
            src_fmt, in0.get_src_n(), in1.get_weight_i(fil_fmt) * g, output_sp);

    set_shape_and_strides(*outputs[0], out0_shape);

    return status::success;
}

status_t infer_conv_bprop_filters_output_shape(op_t *n,
        std::vector<logical_tensor_t *> &inputs,
        std::vector<logical_tensor_t *> &outputs) {
    auto in0 = logical_tensor_wrapper_t(inputs[0]);
    auto in2 = logical_tensor_wrapper_t(inputs[2]);
    auto out0 = logical_tensor_wrapper_t(outputs[0]);

    // get attr value
    const auto &strides = n->get_attr<dims>("strides");
    const auto &pads_begin = n->get_attr<dims>("pads_begin");
    const auto &pads_end = n->get_attr<dims>("pads_end");
    const auto &dilations = n->get_attr<dims>("dilations");
    std::string fil_fmt = n->get_attr<std::string>("filter_format");
    std::string src_fmt = n->get_attr<std::string>("data_format");

    const dims src_dims = in0.vdims();
    const dims output_delta_dims = in2.vdims();

    dims src_sp = in0.get_src_spatial_dims(src_fmt);
    dims output_delta_sp = in2.get_src_spatial_dims(src_fmt);

    // if paddings are empty vectors?
    dims new_pads_begin(pads_begin);
    if (new_pads_begin.empty()) { new_pads_begin.assign(src_sp.size(), 0); }
    dims new_pads_end(pads_end);
    if (new_pads_end.empty()) { new_pads_end.assign(src_sp.size(), 0); }

    // strides and dilations are required and should be correctly provided.
    if (strides.size() != src_sp.size() || dilations.size() != src_sp.size()
            || new_pads_begin.size() != src_sp.size()
            || new_pads_end.size() != src_sp.size()) {
        return status::invalid_shape;
    }

    if (n->has_attr("auto_pad")
            && n->get_attr<std::string>("auto_pad") != "None") {
        std::string auto_pad = n->get_attr<std::string>("auto_pad");

        if (auto_pad == "VALID") {
            size_t rank = src_sp.size();
            new_pads_begin.assign(rank, 0);
            new_pads_end.assign(rank, 0);
        } else if (auto_pad == "SAME_UPPER" || auto_pad == "SAME_LOWER") {
            // Since we have no access to the data of the second input
            // (weights_shape), we cannot calculate auto pads.
            return status::unsupported;
        }

        n->set_attr("pads_begin", new_pads_begin);
        n->set_attr("pads_end", new_pads_end);
    }

    // Since we have no access to the data of the second input (weights_shape),
    // we have to get weights spatial shape using another way.
    // To do that we use transformed convolution output size formula.
    dims fil_sp;
    for (size_t i = 0; i < src_sp.size(); ++i) {
        dim_t padded = src_sp[i] + new_pads_begin[i] + new_pads_end[i];
        dim_t strided = strides[i] * (output_delta_sp[i] - 1);
        fil_sp.push_back(((padded - strided - 1) / dilations[i]) + 1);
    }

    const dims out0_shape = make_filter_dims(
            fil_fmt, in0.get_src_c(src_fmt), in2.get_src_c(src_fmt), fil_sp);

    if (out0.ndims() != -1) {
        if (!validate(out0_shape, out0.vdims())) {
            return status::invalid_shape;
        }
    }

    set_shape_and_strides(*outputs[0], out0_shape);

    return status::success;
}

status_t infer_convtranspose_output_shape(op_t *n,
        std::vector<logical_tensor_t *> &inputs,
        std::vector<logical_tensor_t *> &outputs) {
    auto in0 = logical_tensor_wrapper_t(inputs[0]);
    auto in1 = logical_tensor_wrapper_t(inputs[1]);
    auto out0 = logical_tensor_wrapper_t(outputs[0]);

    // get attr value
    const dim_t g = n->get_attr<dim_t>("groups");
    const auto &strides = n->get_attr<dims>("strides");
    const auto &dilations = n->get_attr<dims>("dilations");
    const auto &pads_begin = n->get_attr<dims>("pads_begin");
    const auto &pads_end = n->get_attr<dims>("pads_end");
    std::string fil_fmt = n->get_attr<std::string>("filter_format");
    std::string src_fmt = n->get_attr<std::string>("data_format");

    if (!out0.is_shape_unknown()) {
        // check if dst channel / groups == weight output channel
        if (out0.get_src_c(src_fmt) / g != in1.get_weight_o(fil_fmt)) {
            return status::invalid_shape;
        }
    }

    dims src_sp = in0.get_src_spatial_dims(src_fmt);
    dims fil_sp = in1.get_weight_spatial_dims(fil_fmt);

    // if paddings are empty vectors?
    dims new_pads_begin(pads_begin);
    if (new_pads_begin.empty()) { new_pads_begin.assign(src_sp.size(), 0); }
    dims new_pads_end(pads_end);
    if (new_pads_end.empty()) { new_pads_end.assign(src_sp.size(), 0); }

    // strides and dilations are required and should be correctly provided.
    if (strides.size() != src_sp.size() || dilations.size() != fil_sp.size()
            || new_pads_begin.size() != src_sp.size()
            || new_pads_end.size() != src_sp.size()) {
        return status::invalid_shape;
    }

    dims output_padding(src_sp.size(), 0);
    if (n->has_attr("output_padding")) {
        output_padding = n->get_attr<dims>("output_padding");
    }

    if (n->has_attr("auto_pad")
            && n->get_attr<std::string>("auto_pad") != "None") {
        std::string auto_pad = n->get_attr<std::string>("auto_pad");

        // infer auto_pad
        for (size_t i = 0; i < src_sp.size(); ++i) {
            infer_auto_pad(src_sp[i], strides[i], fil_sp[i], dilations[i],
                    auto_pad, new_pads_begin[i], new_pads_end[i], true);
        }

        n->set_attr("pads_begin", new_pads_begin);
        n->set_attr("pads_end", new_pads_end);
    }

    dims output_sp;
    for (size_t i = 0; i < src_sp.size(); ++i) {
        dim_t padded = output_padding[i] - new_pads_begin[i] - new_pads_end[i];
        dim_t dilated = dilations[i] * (fil_sp[i] - 1) + 1;
        output_sp.push_back(strides[i] * (src_sp[i] - 1) + dilated + padded);
    }

    const dims out0_shape = make_data_dims(
            src_fmt, in0.get_src_n(), in1.get_weight_o(fil_fmt) * g, output_sp);

    if (out0.ndims() != -1) {
        if (!validate(out0_shape, out0.vdims())) {
            return status::invalid_shape;
        }
    }

    set_shape_and_strides(*outputs[0], out0_shape);

    return status::success;
}

status_t infer_pool_output_shape(op_t *n,
        std::vector<logical_tensor_t *> &inputs,
        std::vector<logical_tensor_t *> &outputs) {
    auto in0 = logical_tensor_wrapper_t(inputs[0]);
    auto out0 = logical_tensor_wrapper_t(outputs[0]);

    // get attr value
    const dims &strides = n->get_attr<dims>("strides");
    const dims &kernel = n->get_attr<dims>("kernel");
    const dims &pads_begin = n->get_attr<dims>("pads_begin");
    const dims &pads_end = n->get_attr<dims>("pads_end");
    std::string rounding_type = "floor";
    if (n->has_attr("rounding_type")) {
        rounding_type = n->get_attr<std::string>("rounding_type");
    }
    std::string src_format = n->get_attr<std::string>("data_format");

    dims dilations(kernel.size(), 1);
    if (n->has_attr("dilations")) {
        auto dilations_tmp = n->get_attr<dims>("dilations");
        dilations_tmp.resize(kernel.size());
        if (dilations_tmp.size() != dilations.size()) {
            return status::invalid_argument;
        } else {
            dilations = dilations_tmp;
        }
    }

    const dims src_dims = in0.vdims();

    dims src_sp = in0.get_src_spatial_dims(src_format);

    // if paddings are empty vectors?
    dims new_pads_begin(pads_begin);
    if (new_pads_begin.empty()) { new_pads_begin.assign(src_sp.size(), 0); }
    dims new_pads_end(pads_end);
    if (new_pads_end.empty()) { new_pads_end.assign(src_sp.size(), 0); }
    if (n->has_attr("auto_pad")
            && n->get_attr<std::string>("auto_pad") != "None") {
        std::string auto_pad = n->get_attr<std::string>("auto_pad");
        // infer auto_pad
        for (size_t i = 0; i < src_sp.size(); ++i) {
            infer_auto_pad(src_sp[i], strides[i], kernel[i], dilations[i],
                    auto_pad, new_pads_begin[i], new_pads_end[i]);
        }
        n->set_attr("pads_begin", new_pads_begin);
        n->set_attr("pads_end", new_pads_end);
    }

    dims output_sp;
    for (size_t i = 0; i < src_sp.size(); ++i) {
        dim_t padded = src_sp[i] + new_pads_begin[i] + new_pads_end[i];
        dim_t dilated = dilations[i] * (kernel[i] - 1) + 1;
        dim_t out_value;
        if (rounding_type == "ceil") {
            out_value = (padded - dilated - 1) / strides[i] + 2;
        } else {
            out_value = (padded - dilated) / strides[i] + 1;
        }
        output_sp.push_back(out_value);
    }

    dims out_shape = make_data_dims(
            src_format, in0.get_src_n(), in0.get_src_c(src_format), output_sp);
    if (out0.ndims() != -1) {
        if (!validate(out_shape, out0.vdims())) {
            return status::invalid_shape;
        }
    }

    set_shape_and_strides(*outputs[0], out_shape);
    return status::success;
}

status_t infer_pool_bwd_output_shape(op_t *n,
        std::vector<logical_tensor_t *> &inputs,
        std::vector<logical_tensor_t *> &outputs) {
    auto in0 = logical_tensor_wrapper_t(inputs[0]);
    auto out0 = logical_tensor_wrapper_t(outputs[0]);

    // check if partial set shape aligns with inferred shape
    if (out0.ndims() != -1) {
        if (!validate(in0.vdims(), out0.vdims())) {
            return status::invalid_shape;
        }
    }

    // We should compute output dense strides instead of
    // directly copying input strides to it
    set_shape_and_strides(*outputs[0], in0.vdims());

    // get attr value
    const dims &strides = n->get_attr<dims>("strides");
    const dims &kernel = n->get_attr<dims>("kernel");
    const dims &pads_begin = n->get_attr<dims>("pads_begin");
    const dims &pads_end = n->get_attr<dims>("pads_end");
    std::string src_format = n->get_attr<std::string>("data_format");

    dims dilations(kernel.size(), 1);
    if (n->has_attr("dilations")) {
        auto dilations_tmp = n->get_attr<dims>("dilations");
        if (dilations_tmp.size() != dilations.size()) {
            return status::invalid_argument;
        } else {
            dilations = dilations_tmp;
        }
    }

    const dims src_dims = in0.vdims();

    dims src_sp = in0.get_src_spatial_dims(src_format);

    // if paddings are empty vectors?
    dims new_pads_begin(pads_begin);
    if (new_pads_begin.empty()) { new_pads_begin.assign(src_sp.size(), 0); }
    dims new_pads_end(pads_end);
    if (new_pads_end.empty()) { new_pads_end.assign(src_sp.size(), 0); }
    if (n->has_attr("auto_pad")
            && n->get_attr<std::string>("auto_pad") != "None") {
        std::string auto_pad = n->get_attr<std::string>("auto_pad");
        // infer auto_pad
        for (size_t i = 0; i < src_sp.size(); ++i) {
            infer_auto_pad(src_sp[i], strides[i], kernel[i], dilations[i],
                    auto_pad, new_pads_begin[i], new_pads_end[i]);
        }
        n->set_attr("pads_begin", new_pads_begin);
        n->set_attr("pads_end", new_pads_end);
    }

    return status::success;
}

status_t infer_matmul_output_shape(op_t *n,
        std::vector<logical_tensor_t *> &inputs,
        std::vector<logical_tensor_t *> &outputs) {
    auto in0 = logical_tensor_wrapper_t(inputs[0]);
    auto in1 = logical_tensor_wrapper_t(inputs[1]);
    auto out0 = logical_tensor_wrapper_t(outputs[0]);

    // get attr value
    bool transpose_a = false;
    if (n->has_attr("transpose_a")) {
        transpose_a = n->get_attr<bool>("transpose_a");
    }
    bool transpose_b = false;
    if (n->has_attr("transpose_b")) {
        transpose_b = n->get_attr<bool>("transpose_b");
    }
    const dims input0_dims = in0.vdims();
    const dims input1_dims = in1.vdims();
    size_t input0_rank = input0_dims.size();
    size_t input1_rank = input1_dims.size();
    dims updated_input0(input0_dims);
    dims updated_input1(input1_dims);
    if (transpose_a && input0_rank > 1) {
        std::swap(updated_input0[input0_rank - 2],
                updated_input0[input0_rank - 1]);
    }
    if (transpose_b && input1_rank > 1) {
        std::swap(updated_input1[input1_rank - 2],
                updated_input1[input1_rank - 1]);
    }

    dims inferred_out_shape;
    if (input0_rank == 1 && input1_rank == 1) {
        if (updated_input0 != updated_input1) {
            // matmul: incompatible arg shapes
            return status::invalid_shape;
        }
        // example: input0={1,1}, input1={1,1}, output={2}
        inferred_out_shape = {1};
    } else if (input0_rank == 1) {
        if (updated_input0[0] != updated_input1[input1_rank - 2]) {
            // matmul: incompatible arg shapes
            return status::invalid_shape;
        }
        // example: input0 shape {3}, input1 shape {2,3,4},
        // output shape {2,4}
        updated_input1.erase(
                updated_input1.begin() + static_cast<dim_t>(input1_rank) - 2);
        inferred_out_shape = updated_input1;
    } else if (input1_rank == 1) {
        if (updated_input1[0] != updated_input0[input0_rank - 1]) {
            // matmul: incompatible arg shapes
            return status::invalid_shape;
        }
        // example: input0 shape {2,3,4}, input1 shape {4},
        // output shape {2,3}
        updated_input0.erase(
                updated_input0.begin() + static_cast<dim_t>(input0_rank) - 1);
        inferred_out_shape = updated_input0;
    } else if (input0_rank == 2 && input1_rank == 2) {
        if (updated_input0[1] != updated_input1[0]) {
            // matmul: incompatible arg shapes
            return status::invalid_shape;
        }
        // example: input0 shape {1, 3}, input1 shape {3, 2},
        // output shape {1,2}
        inferred_out_shape = {updated_input0[0], updated_input1[1]};
    } else {
        if (updated_input0[input0_rank - 1]
                != updated_input1[input1_rank - 2]) {
            // matmul: incompatible arg shapes
            return status::invalid_shape;
        }
        std::vector<int64_t> input0_batch_dims {
                updated_input0.begin(), updated_input0.end() - 2};
        std::vector<int64_t> input1_batch_dims {
                updated_input1.begin(), updated_input1.end() - 2};
        status_t ret = broadcast(
                input0_batch_dims, input1_batch_dims, inferred_out_shape);
        if (ret != status::success) { return ret; }
        inferred_out_shape.push_back(updated_input0[input0_rank - 2]);
        inferred_out_shape.push_back(updated_input1[input1_rank - 1]);
    }

    if (out0.ndims() != -1) {
        if (!validate(inferred_out_shape, out0.vdims())) {
            return status::invalid_shape;
        }
    }

    set_shape_and_strides(*outputs[0], inferred_out_shape);
    return status::success;
}

status_t infer_identity_output_shape(op_t *n,
        std::vector<logical_tensor_t *> &inputs,
        std::vector<logical_tensor_t *> &outputs) {
    auto out0 = logical_tensor_wrapper_t(outputs[0]);
    auto in0 = logical_tensor_wrapper_t(inputs[0]);

    // check if partial set shape aligns with inferred shape
    if (out0.ndims() != -1) {
        if (!validate(in0.vdims(), out0.vdims())) {
            return status::invalid_shape;
        }
    }

    // We should compute output dense strides instead of
    // directly copying input strides to it
    set_shape_and_strides(*outputs[0], in0.vdims());
    UNUSED(n);
    return status::success;
}

status_t identity_output_shape_on_pos(op_t *n,
        std::vector<logical_tensor_t *> &inputs,
        std::vector<logical_tensor_t *> &outputs,
        std::vector<std::pair<uint32_t, uint32_t>> &positions) {
    for (auto &pos : positions) {
        std::vector<logical_tensor_t *> ins = {inputs[pos.first]};
        std::vector<logical_tensor_t *> outs = {outputs[pos.second]};
        auto status = infer_identity_output_shape(n, ins, outs);
        if (status != status::success) return status;
    }
    return status::success;
}

status_t infer_bias_backprop_output_shape(op_t *n,
        std::vector<logical_tensor_t *> &inputs,
        std::vector<logical_tensor_t *> &outputs) {
    auto out = logical_tensor_wrapper_t(outputs[0]);
    if (!out.is_shape_unknown()) return status::success;

    auto in = logical_tensor_wrapper_t(inputs[0]);
    dims input_dims = in.vdims();
    if (input_dims.size() < 4) {
        // bias add backprop: input should have at least 4 dims.
        return status::invalid_shape;
    }

    std::string fmt = n->has_attr("data_format")
            ? n->get_attr<std::string>("data_format")
            : "NXC";

    const auto channels = in.get_src_c(fmt);
    dims new_out_dims = {channels};

    set_shape_and_strides(*outputs[0], new_out_dims);
    return status::success;
}

status_t infer_bias_add_output_shape(op_t *n,
        std::vector<logical_tensor_t *> &inputs,
        std::vector<logical_tensor_t *> &outputs) {
    auto out = logical_tensor_wrapper_t(outputs[0]);
    if (!out.is_shape_unknown()) return status::success;

    auto in = logical_tensor_wrapper_t(inputs[0]);
    dims input_dims = in.vdims();
    if (input_dims.size() < 4) {
        // bias add: input should have at least 4 dims.
        return status::invalid_shape;
    }
    auto bias = logical_tensor_wrapper_t(inputs[1]);
    dims bias_dims = bias.vdims();
    if (bias_dims.size() != 1) {
        // bias add: bias should have exactly 1 dimension.
        return status::invalid_shape;
    }

    // following the spec of convolution, nxc as default format
    std::string fmt = n->has_attr("data_format")
            ? n->get_attr<std::string>("data_format")
            : "NXC";

    const auto channels = in.get_src_c(fmt);
    if (bias_dims[0] != channels) {
        // bias add: bias size should match input channels size.
        return status::invalid_shape;
    }

    return infer_identity_output_shape(n, inputs, outputs);
}

status_t infer_norm_output_shape(op_t *n,
        std::vector<logical_tensor_t *> &inputs,
        std::vector<logical_tensor_t *> &outputs) {
    auto status = infer_identity_output_shape(n, inputs, outputs);
    if (status != status::success) return status;

    const bool keep_stats = n->has_attr("keep_stats")
            ? n->get_attr<bool>("keep_stats")
            : false;
    if (!keep_stats) return status::success;

    auto in0 = logical_tensor_wrapper_t(inputs[0]);
    const dims input0_dims = in0.vdims();

    const dim_t begin_norm_axis = n->has_attr("begin_norm_axis")
            ? n->get_attr<dim_t>("begin_norm_axis")
            : -1;

    auto out1 = logical_tensor_wrapper_t(outputs[1]);
    auto out2 = logical_tensor_wrapper_t(outputs[2]);
    dims output_dims(input0_dims);

    auto norm_starting_position
            = begin_norm_axis >= 0 ? output_dims.begin() : output_dims.end();

    output_dims.erase(
            norm_starting_position + begin_norm_axis, output_dims.end());

    // check if output shape is already known
    if (out1.is_shape_unknown()) {
        set_shape_and_strides(*outputs[1], output_dims);
    }

    // check if output shape is already known
    if (out2.is_shape_unknown()) {
        set_shape_and_strides(*outputs[2], output_dims);
    }
    return status::success;
}

status_t infer_norm_bprop_output_shape(op_t *n,
        std::vector<logical_tensor_t *> &inputs,
        std::vector<logical_tensor_t *> &outputs) {
    std::vector<std::pair<uint32_t, uint32_t>> identity_shapes_pos = {{0, 0}};
    if (n->has_attr("use_affine") && n->get_attr<bool>("use_affine") == true) {
        // when use_affine parameter is set,
        // there will be two additional outputs
        identity_shapes_pos.insert(identity_shapes_pos.end(), {{4, 1}, {5, 2}});
    }
    return identity_output_shape_on_pos(
            n, inputs, outputs, identity_shapes_pos);
}

status_t infer_elemwise_arithmetic_output_shape(op_t *n,
        std::vector<logical_tensor_t *> &inputs,
        std::vector<logical_tensor_t *> &outputs) {
    auto in0 = logical_tensor_wrapper_t(inputs[0]);
    auto in1 = logical_tensor_wrapper_t(inputs[1]);
    // check if output shape is already known
    auto out0 = logical_tensor_wrapper_t(outputs[0]);
    if (!out0.is_shape_unknown()) return status::success;

    const std::string broadcast_attr_name = "auto_broadcast";
    const bool shapes_should_match = [n, &broadcast_attr_name]() {
        if (n->has_attr(broadcast_attr_name)) {
            const auto &auto_broadcast
                    = n->get_attr<std::string>(broadcast_attr_name);
            return auto_broadcast == "none";
        }
        return false;
    }();

    dims input0_dims = in0.vdims();
    dims input1_dims = in1.vdims();
    dims inferred_out_shape;
    if (shapes_should_match) {
        if (input0_dims != input1_dims) {
            // add: incompatible input shapes (auto_broadcast=none)
            return status::invalid_shape;
        }
        inferred_out_shape = input0_dims;
    } else {
        status_t ret = broadcast(input0_dims, input1_dims, inferred_out_shape);
        if (ret != status::success) { return ret; }
    }
    // check if partial set shape aligns with inferred shape
    if (out0.ndims() != -1) {
        if (!validate(inferred_out_shape, out0.vdims())) {
            return status::invalid_shape;
        }
    }

    set_shape_and_strides(*outputs[0], inferred_out_shape);
    return status::success;
}

status_t infer_bn_fwd_train_output_shape(op_t *n,
        std::vector<logical_tensor_t *> &inputs,
        std::vector<logical_tensor_t *> &outputs) {
    using cvec_int64 = const std::vector<int64_t>;

    if (every_shape_is_known(outputs)) return status::success;

    const auto in = logical_tensor_wrapper_t(inputs[0]);
    cvec_int64 input_dims = in.vdims();
    if (input_dims.size() < 4) return status::invalid_shape;

    std::string fmt = n->has_attr("data_format")
            ? n->get_attr<std::string>("data_format")
            : "NXC";

    const auto channels = in.get_src_c(fmt);
    const auto validator = [&channels](cvec_int64 &vec) {
        return vec.size() == 1 && vec[0] == channels;
    };
    if (!verify_shapes_in_range(inputs, 1, inputs.size(), validator))
        return status::invalid_shape;

    infer_identity_output_shape(n, inputs, outputs);
    cvec_int64 new_out_dims = {channels};
    set_shapes_in_range(outputs, 1, outputs.size(), new_out_dims);
    return status::success;
}

status_t infer_bn_bwd_output_shape(op_t *n,
        std::vector<logical_tensor_t *> &inputs,
        std::vector<logical_tensor_t *> &outputs) {
    using cvec_int64 = const std::vector<int64_t>;

    if (every_shape_is_known(outputs)) return status::success;

    const auto in = logical_tensor_wrapper_t(inputs[0]);
    cvec_int64 input_dims = in.vdims();
    const auto out_delta = logical_tensor_wrapper_t(inputs[1]);
    cvec_int64 out_delta_dims = out_delta.vdims();
    if (input_dims.size() < 4 || out_delta_dims.size() < 4)
        return status::invalid_shape;

    std::string fmt = n->has_attr("data_format")
            ? n->get_attr<std::string>("data_format")
            : "NXC";

    const auto channels = in.get_src_c(fmt);
    const auto validator = [&channels](cvec_int64 &vec) {
        return vec.size() == 1 && vec[0] == channels;
    };
    if (!verify_shapes_in_range(inputs, 2, inputs.size(), validator))
        return status::invalid_shape;

    infer_identity_output_shape(n, inputs, outputs);
    cvec_int64 new_out_dims = {channels};
    set_shapes_in_range(outputs, 1, outputs.size(), new_out_dims);
    return status::success;
}

status_t infer_concat_output_shape(op_t *n,
        std::vector<logical_tensor_t *> &inputs,
        std::vector<logical_tensor_t *> &outputs) {
    auto out0 = logical_tensor_wrapper_t(outputs[0]);
    if (!out0.is_shape_unknown()) return status::success;

    // if only one tensor to concat, out_shape is same as input_shape
    if (inputs.size() == 1) {
        infer_identity_output_shape(n, inputs, outputs);
        return status::success;
    }
    auto in0 = logical_tensor_wrapper_t(inputs[0]);
    auto data_type = in0.data_type();
    if (data_type != out0.data_type()) return status::unsupported;

    int64_t axis = n->get_attr<int64_t>("axis");
    auto ndims = in0.ndims();
    auto dims = in0.dims();
    if (axis < -ndims || axis >= ndims) {
        return status::invalid_argument;
    } else if (axis < 0) {
        axis += ndims;
    }

    int64_t sum = 0;
    for (auto iter = inputs.cbegin(); iter != inputs.cend(); iter++) {
        auto lt_inN = logical_tensor_wrapper_t(*iter);
        const auto &lt_inN_dims = lt_inN.vdims();
        if (lt_inN.ndims() != ndims) { return status::invalid_shape; }
        if (lt_inN.data_type() != data_type) { return status::unsupported; }
        for (int32_t i = 0; i < ndims; i++) {
            if (i != axis) {
                // input dims should be same except axis dim.
                if (dims[i] != lt_inN_dims[static_cast<size_t>(i)]) {
                    return status::invalid_shape;
                }
            } else {
                sum += lt_inN_dims[static_cast<size_t>(axis)];
            }
        }
    };

    std::vector<int64_t> infered_out_shape(dims, dims + ndims);
    infered_out_shape[axis] = sum;
    set_shape_and_strides(*outputs[0], infered_out_shape);
    return status::success;
}

status_t infer_unsupported_output_shape(op_t *n,
        std::vector<logical_tensor_t *> &inputs,
        std::vector<logical_tensor_t *> &outputs) {
    UNUSED(n);
    UNUSED(inputs);
    auto out0 = logical_tensor_wrapper_t(outputs[0]);
    if (out0.is_shape_unknown()) return status::unsupported;
    return status::success;
}

/// Shape inference function for PowBackpropExponent
status_t infer_exponent_output_shape(op_t *n,
        std::vector<logical_tensor_t *> &inputs,
        std::vector<logical_tensor_t *> &outputs) {
    UNUSED(n);
    auto out0 = logical_tensor_wrapper_t(outputs[0]); // exponent_delta
    if (!out0.is_shape_unknown()) return status::success;

    auto in = logical_tensor_wrapper_t(inputs[3]); // exponent
    auto dims = in.vdims();
    set_shape_and_strides(*outputs[0], dims);
    return status::success;
}

status_t infer_reduce_output_shape(op_t *n,
        std::vector<logical_tensor_t *> &inputs,
        std::vector<logical_tensor_t *> &outputs) {
    auto out0 = logical_tensor_wrapper_t(outputs[0]);
    // check if output shape is already known
    if (!out0.is_shape_unknown()) return status::success;

    if (n->has_attr("axes")) {
        auto axes = n->get_attr<dims>("axes");
        // our backend doesn't support such a case
        if (axes.empty()) return status::unsupported;

        auto shape = logical_tensor_wrapper_t(inputs[0]).vdims();
        auto ndim = static_cast<int64_t>(shape.size());

        if (std::any_of(axes.begin(), axes.end(), [&ndim](int64_t axis) {
                return axis < -ndim || axis >= ndim;
            }))
            return status::unsupported;

        // convert negative axis to positive one
        std::transform(axes.begin(), axes.end(), axes.begin(),
                [&ndim](int64_t axis) -> int64_t {
                    return axis < 0 ? ndim + axis : axis;
                });

        if (std::unordered_set<int64_t>(axes.begin(), axes.end()).size()
                < axes.size())
            return status::unsupported;

        auto keep_dims = n->has_attr("keep_dims")
                ? n->get_attr<bool>("keep_dims")
                : false;
        for (auto axis : axes)
            shape[static_cast<size_t>(axis)] = (keep_dims) ? 1 : 0;
        if (!keep_dims)
            shape.erase(std::remove_if(shape.begin(), shape.end(),
                                [](int64_t d) { return d == 0; }),
                    shape.end());
        if (shape.empty()) shape.push_back(1);

        set_shape_and_strides(*outputs[0], shape);

        return status::success;
    }

    // When the second input is an empty list,
    // then this operation does nothing, it is an identity.
    // For dims.size() == 0, we set ndims to -1 and can't get
    // vdims. Therefore, this path is not supported.

    // since we don't have an access to the second input data,
    // which contain axis indices, we cannot calculate output shape
    return status::unsupported;
}

status_t infer_static_reshape_output_shape(op_t *n,
        std::vector<logical_tensor_t *> &inputs,
        std::vector<logical_tensor_t *> &outputs) {
    auto out0 = logical_tensor_wrapper_t(outputs[0]);
    auto in0 = logical_tensor_wrapper_t(inputs[0]);
    if (!out0.is_shape_unknown()) return status::success;

    // check if partial set shape aligns with inferred shape
    if (out0.ndims() != -1) {
        if (!validate(in0.vdims(), out0.vdims())) {
            return status::invalid_shape;
        }
    }

    const dims &in_dims = in0.vdims();
    dims out_dims = n->get_attr<dims>("shape");
    const bool special_zero = n->get_attr<bool>("special_zero");

    bool find_uncertain_dim = false; // shape contains -1
    size_t uncertain_axis = 0;
    for (size_t i = 0; i < out_dims.size(); i++) {
        if (out_dims[i] < -1) return status::invalid_shape;
        if (out_dims[i] == 0) {
            // handle special_zero: 0 means same as input shape in that
            // dimension if special_zero is true; or 0 as-is if special_zero is
            // false
            if (special_zero) {
                if (i >= in0.ndims()) return status::invalid_shape;
                out_dims[i] = in_dims[i];
            }
        } else if (out_dims[i] == -1) {
            // only allow at most one -1
            if (find_uncertain_dim) return status::invalid_shape;
            find_uncertain_dim = true;
            uncertain_axis = i;
        }
    }

    int in_size = 1;
    int out_size = 1;
    for (size_t i = 0; i < in0.ndims(); i++) {
        if (in_dims[i] >= 0) in_size *= in_dims[i];
    }
    for (size_t i = 0; i < out_dims.size(); i++) {
        if (out_dims[i] >= 0) out_size *= out_dims[i];
    }
    // handle -1 in output shape: the value is inferred from the size of the
    // input tensor and the remaining dimensions
    if (find_uncertain_dim) {
        if (out_size == 0) return status::invalid_shape;
        out_dims[uncertain_axis] = in_size / out_size;
    }

    // size of input should be same as output
    if (find_uncertain_dim == false) {
        if (out_size != in_size) return status::invalid_shape;
    } else {
        if (out_size * out_dims[uncertain_axis] != in_size)
            return status::invalid_shape;
    }

    // We should compute output dense strides instead of
    // directly copying input strides to it
    set_shape_and_strides(*outputs[0], out_dims);
    return status::success;
}

status_t infer_static_transpose_output_shape(op_t *n,
        std::vector<logical_tensor_t *> &inputs,
        std::vector<logical_tensor_t *> &outputs) {
    auto out0 = logical_tensor_wrapper_t(outputs[0]);
    auto in0 = logical_tensor_wrapper_t(inputs[0]);
    if (!out0.is_shape_unknown()) return status::success;

    // check if partial set shape aligns with inferred shape
    if (out0.ndims() != -1) {
        if (!validate(in0.vdims(), out0.vdims())) {
            return status::invalid_shape;
        }
    }

    const dims &in_dims = in0.vdims();
    const int32_t in_ndims = in0.ndims();
    std::vector<int64_t> order = n->get_attr<dims>("order");
    std::vector<bool> order_covered_flg(in_ndims, false);
    // check order should be in [-n, n-1] and cover all input axis
    // if order < 0, convert it to postive order
    if (!order.empty()) {
        if (order.size() != in_ndims) return status::invalid_shape;
        for (int64_t &axis : order) {
            if (axis < -in_ndims || axis > in_ndims - 1)
                return status::invalid_shape;
            if (axis < 0) axis += in_ndims;
            if (order_covered_flg[axis]) {
                return status::invalid_shape;
            } else {
                order_covered_flg[axis] = true;
            }
        }
    }

    dims out_dims;
    out_dims.reserve(in_ndims);
    if (order.empty()) {
        // If order is not given, will transpose to (n-1...0),
        for (int i = in_ndims - 1; i >= 0; --i)
            out_dims.push_back(in_dims[i]);
    } else {
        for (const int64_t &axis : order) {
            out_dims.push_back(
                    axis >= 0 ? in_dims[axis] : in_dims[axis + in_ndims]);
        }
    }
    // We should compute output dense strides instead of
    // directly copying input strides to it
    set_shape_and_strides(*outputs[0], out_dims);
    return status::success;
}

status_t infer_interpolate_output_shape(op_t *n,
        std::vector<logical_tensor_t *> &inputs,
        std::vector<logical_tensor_t *> &outputs) {
    auto in = logical_tensor_wrapper_t(inputs[0]);
    auto in_dims = in.vdims();
    // Number of spatial dimensions
    int spatial_ndim = in.ndims() - 2;
    auto out0 = logical_tensor_wrapper_t(outputs[0]);
    if (!out0.is_shape_unknown()) return status::success;

    std::vector<int64_t> sizes;
    if (n->has_attr("sizes")) {
        // sizes is set by user
        sizes = n->get_attr<std::vector<int64_t>>("sizes");
    }
    std::vector<float> scales;
    if (n->has_attr("scales")) {
        // scales is set by user
        scales = n->get_attr<std::vector<float>>("scales");
    }
    if ((!scales.empty() && !sizes.empty())
            || (scales.empty() && sizes.empty())) {
        // only one of them should be not none
        return status::invalid_argument;
    }

    std::string src_fmt = n->get_attr<std::string>("data_format");
    int spatial_dim_start_axis = 0;
    if (src_fmt == "NXC") {
        // "X" start from in_dims[1]
        spatial_dim_start_axis = 1;
    } else if (src_fmt == "NCX") {
        // "X" start from in_dims[2]
        spatial_dim_start_axis = 2;
    } else {
        return status::invalid_argument;
    }

    if (!scales.empty()) {
        // scales length should equal spatial_ndim
        if (scales.size() != spatial_ndim) return status::invalid_argument;

        // spatial_ndim
        for (size_t i = 0; i < spatial_ndim; i++) {
            in_dims[i + spatial_dim_start_axis] *= scales[i];
        }
    }
    if (!sizes.empty()) {
        // sizes length should equal spatial_ndim
        if (sizes.size() != spatial_ndim) return status::invalid_argument;

        for (size_t i = 0; i < spatial_ndim; i++) {
            in_dims[i + spatial_dim_start_axis] = sizes[i];
        }
    }
    set_shape_and_strides(*outputs[0], in_dims);
    return status::success;
}
} // namespace impl
} // namespace graph
} // namespace dnnl