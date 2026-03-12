// Copyright (c) 2024-2026 Lux Industries Inc.
// SPDX-License-Identifier: BSD-3-Clause

#define LUX_ACCEL_BUILDING
#include <lux/accel/c_api.h>
#include <lux/accel/backend_api.h>
#include <lux/accel/accel.hpp>

#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

// Thread-local error message
thread_local std::string g_last_error;

// Global state
std::mutex g_mutex;
bool g_initialized = false;
std::vector<void*> g_loaded_plugins;
std::vector<lux_backend_interface_t*> g_backends;

// Core API provided to plugins
lux_core_api_t g_core_api;

void set_error(const char* msg) {
    g_last_error = msg ? msg : "Unknown error";
}

lux_status to_status(const lux::gpu::Status& status) {
    if (status.ok()) return LUX_OK;
    switch (status.code) {
        case lux::gpu::StatusCode::OutOfMemory: return LUX_OUT_OF_MEMORY;
        case lux::gpu::StatusCode::InvalidArgument: return LUX_INVALID_ARGUMENT;
        case lux::gpu::StatusCode::NotSupported: return LUX_NOT_SUPPORTED;
        case lux::gpu::StatusCode::BackendNotAvailable: return LUX_NO_BACKEND;
        case lux::gpu::StatusCode::KernelCompilationFailed:
        case lux::gpu::StatusCode::KernelNotFound: return LUX_KERNEL_ERROR;
        case lux::gpu::StatusCode::DispatchFailed: return LUX_DISPATCH_FAILED;
        default: return LUX_ERROR;
    }
}

// Core API implementations
void core_log_debug(const char* msg) { /* TODO: proper logging */ (void)msg; }
void core_log_info(const char* msg) { /* TODO: proper logging */ (void)msg; }
void core_log_warn(const char* msg) { /* TODO: proper logging */ (void)msg; }
void core_log_error(const char* msg) { set_error(msg); }
void* core_alloc(size_t size) { return std::malloc(size); }
void core_free(void* ptr) { std::free(ptr); }
const void* core_get_kernel_bundle(const char* name, size_t* size) {
    (void)name; (void)size;
    return nullptr; // TODO: implement kernel bundle registry
}
const char* core_get_kernel_source(const char* name) {
    (void)name;
    return nullptr; // TODO: implement kernel source registry
}

void init_core_api() {
    g_core_api.api_version = LUX_BACKEND_API_VERSION;
    g_core_api.log_debug = core_log_debug;
    g_core_api.log_info = core_log_info;
    g_core_api.log_warn = core_log_warn;
    g_core_api.log_error = core_log_error;
    g_core_api.alloc = core_alloc;
    g_core_api.free = core_free;
    g_core_api.get_kernel_bundle = core_get_kernel_bundle;
    g_core_api.get_kernel_source = core_get_kernel_source;
}

} // anonymous namespace

// =============================================================================
// Internal session wrapper
// =============================================================================

struct lux_session_t {
    std::unique_ptr<lux::accel::Session> session;
    std::unordered_map<lux_tensor, std::unique_ptr<lux::gpu::Buffer>> tensors;
};

struct lux_tensor_t {
    lux::gpu::BufferPtr buffer;
    std::vector<size_t> shape;
    lux_dtype dtype;
};

// =============================================================================
// Library initialization
// =============================================================================

extern "C" {

LUX_API lux_status lux_init(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_initialized) return LUX_OK;

    init_core_api();
    g_initialized = true;
    return LUX_OK;
}

LUX_API void lux_shutdown(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized) return;

    // Unload plugins
    for (auto* backend : g_backends) {
        if (backend && backend->shutdown) {
            backend->shutdown();
        }
    }
    g_backends.clear();

    for (auto handle : g_loaded_plugins) {
#ifdef _WIN32
        FreeLibrary((HMODULE)handle);
#else
        dlclose(handle);
#endif
    }
    g_loaded_plugins.clear();

    g_initialized = false;
}

LUX_API const char* lux_version(void) {
    return "0.1.0";
}

LUX_API const char* lux_get_error(void) {
    return g_last_error.c_str();
}

// =============================================================================
// Backend management
// =============================================================================

LUX_API lux_status lux_load_backend(const char* path) {
    if (!path) {
        set_error("Invalid plugin path");
        return LUX_INVALID_ARGUMENT;
    }

#ifdef _WIN32
    HMODULE handle = LoadLibraryA(path);
    if (!handle) {
        set_error("Failed to load plugin");
        return LUX_ERROR;
    }
    auto init_fn = (lux_backend_init_fn)GetProcAddress(handle, LUX_BACKEND_INIT_SYMBOL);
#else
    void* handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        set_error(dlerror());
        return LUX_ERROR;
    }
    auto init_fn = (lux_backend_init_fn)dlsym(handle, LUX_BACKEND_INIT_SYMBOL);
#endif

    if (!init_fn) {
        set_error("Plugin missing lux_backend_init symbol");
#ifdef _WIN32
        FreeLibrary(handle);
#else
        dlclose(handle);
#endif
        return LUX_ERROR;
    }

    lux_backend_interface_t* backend = init_fn(&g_core_api);
    if (!backend) {
        set_error("Plugin initialization failed");
#ifdef _WIN32
        FreeLibrary(handle);
#else
        dlclose(handle);
#endif
        return LUX_ERROR;
    }

    if (backend->api_version != LUX_BACKEND_API_VERSION) {
        set_error("Plugin API version mismatch");
#ifdef _WIN32
        FreeLibrary(handle);
#else
        dlclose(handle);
#endif
        return LUX_ERROR;
    }

    if (backend->init && !backend->init()) {
        set_error("Backend initialization failed");
#ifdef _WIN32
        FreeLibrary(handle);
#else
        dlclose(handle);
#endif
        return LUX_ERROR;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    g_loaded_plugins.push_back(handle);
    g_backends.push_back(backend);

    return LUX_OK;
}

LUX_API int lux_backend_count(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return static_cast<int>(g_backends.size());
}

LUX_API lux_backend_type lux_backend_type_at(int index) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (index < 0 || index >= static_cast<int>(g_backends.size())) {
        return LUX_BACKEND_AUTO;
    }
    return static_cast<lux_backend_type>(g_backends[index]->type);
}

LUX_API int lux_device_count(lux_backend_type backend) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto* b : g_backends) {
        if (static_cast<lux_backend_type>(b->type) == backend) {
            return b->get_device_count ? b->get_device_count() : 0;
        }
    }
    return 0;
}

LUX_API lux_status lux_get_device_info(lux_backend_type backend, int index, lux_device_info* info) {
    if (!info) return LUX_INVALID_ARGUMENT;

    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto* b : g_backends) {
        if (static_cast<lux_backend_type>(b->type) == backend) {
            if (!b->get_device_caps) return LUX_NOT_SUPPORTED;

            lux_device_caps_t caps;
            if (!b->get_device_caps(index, &caps)) {
                return LUX_ERROR;
            }

            info->name = caps.name;
            info->vendor = caps.vendor;
            info->backend = backend;
            info->is_discrete = caps.is_discrete;
            info->is_unified_memory = caps.is_unified_memory;
            info->total_memory = caps.total_memory;
            info->max_workgroup_size = caps.max_workgroup_size;
            return LUX_OK;
        }
    }
    return LUX_NO_BACKEND;
}

// =============================================================================
// Session management
// =============================================================================

LUX_API lux_status lux_session_create(lux_session* session) {
    if (!session) return LUX_INVALID_ARGUMENT;

    auto result = lux::accel::Session::create();
    if (!result) {
        set_error(result.error().message.c_str());
        return to_status(result.error());
    }

    auto* s = new lux_session_t();
    s->session = std::move(*result);
    *session = s;
    return LUX_OK;
}

LUX_API lux_status lux_session_create_with_backend(lux_backend_type backend, lux_session* session) {
    if (!session) return LUX_INVALID_ARGUMENT;

    lux::gpu::BackendType type;
    switch (backend) {
        case LUX_BACKEND_METAL: type = lux::gpu::BackendType::Metal; break;
        case LUX_BACKEND_WEBGPU: type = lux::gpu::BackendType::WebGPU; break;
        case LUX_BACKEND_CUDA: type = lux::gpu::BackendType::CUDA; break;
        default: type = lux::gpu::BackendType::Auto; break;
    }

    auto result = lux::accel::Session::create(type);
    if (!result) {
        set_error(result.error().message.c_str());
        return to_status(result.error());
    }

    auto* s = new lux_session_t();
    s->session = std::move(*result);
    *session = s;
    return LUX_OK;
}

LUX_API lux_status lux_session_create_with_device(lux_backend_type backend, int device_index,
                                                   lux_session* session) {
    // TODO: implement device selection
    (void)device_index;
    return lux_session_create_with_backend(backend, session);
}

LUX_API void lux_session_destroy(lux_session session) {
    delete session;
}

LUX_API lux_status lux_session_sync(lux_session session) {
    if (!session || !session->session) return LUX_INVALID_ARGUMENT;
    auto status = session->session->sync();
    if (!status.ok()) {
        set_error(status.message.c_str());
        return to_status(status);
    }
    return LUX_OK;
}

LUX_API lux_status lux_session_get_device_info(lux_session session, lux_device_info* info) {
    if (!session || !session->session || !info) return LUX_INVALID_ARGUMENT;

    const auto& dev_info = session->session->device().info();
    info->name = dev_info.name.c_str();
    info->vendor = dev_info.vendor.c_str();
    info->backend = static_cast<lux_backend_type>(dev_info.backend);
    info->is_discrete = dev_info.is_discrete ? 1 : 0;
    info->is_unified_memory = dev_info.is_unified_memory ? 1 : 0;
    info->total_memory = dev_info.total_memory;
    info->max_workgroup_size = dev_info.features.max_workgroup_size;
    return LUX_OK;
}

// =============================================================================
// Tensor operations
// =============================================================================

static size_t dtype_size(lux_dtype dtype) {
    switch (dtype) {
        case LUX_DTYPE_F32: return 4;
        case LUX_DTYPE_F16: return 2;
        case LUX_DTYPE_F64: return 8;
        case LUX_DTYPE_I32: return 4;
        case LUX_DTYPE_I64: return 8;
        case LUX_DTYPE_U8: return 1;
        case LUX_DTYPE_U32: return 4;
        case LUX_DTYPE_U64: return 8;
        default: return 0;
    }
}

LUX_API lux_status lux_tensor_create(lux_session session, lux_dtype dtype,
                                      const size_t* shape, size_t ndim,
                                      lux_tensor* tensor) {
    if (!session || !session->session || !shape || !tensor) return LUX_INVALID_ARGUMENT;

    size_t numel = 1;
    for (size_t i = 0; i < ndim; i++) numel *= shape[i];
    size_t bytes = numel * dtype_size(dtype);

    auto result = session->session->device().createBuffer(
        bytes, lux::gpu::BufferUsage::Storage | lux::gpu::BufferUsage::MapRead |
               lux::gpu::BufferUsage::MapWrite);

    if (!result) {
        set_error(result.error().message.c_str());
        return to_status(result.error());
    }

    auto* t = new lux_tensor_t();
    t->buffer = std::move(*result);
    t->shape.assign(shape, shape + ndim);
    t->dtype = dtype;
    *tensor = t;
    return LUX_OK;
}

LUX_API lux_status lux_tensor_create_with_data(lux_session session, lux_dtype dtype,
                                                const size_t* shape, size_t ndim,
                                                const void* data, size_t data_bytes,
                                                lux_tensor* tensor) {
    if (!session || !session->session || !shape || !data || !tensor) return LUX_INVALID_ARGUMENT;

    size_t numel = 1;
    for (size_t i = 0; i < ndim; i++) numel *= shape[i];
    size_t expected_bytes = numel * dtype_size(dtype);

    if (data_bytes != expected_bytes) return LUX_INVALID_ARGUMENT;

    auto result = session->session->device().createBuffer(
        std::span<const uint8_t>(static_cast<const uint8_t*>(data), data_bytes),
        lux::gpu::BufferUsage::Storage | lux::gpu::BufferUsage::MapRead |
        lux::gpu::BufferUsage::MapWrite);

    if (!result) {
        set_error(result.error().message.c_str());
        return to_status(result.error());
    }

    auto* t = new lux_tensor_t();
    t->buffer = std::move(*result);
    t->shape.assign(shape, shape + ndim);
    t->dtype = dtype;
    *tensor = t;
    return LUX_OK;
}

LUX_API void lux_tensor_destroy(lux_tensor tensor) {
    delete tensor;
}

LUX_API size_t lux_tensor_ndim(lux_tensor tensor) {
    return tensor ? tensor->shape.size() : 0;
}

LUX_API size_t lux_tensor_shape(lux_tensor tensor, size_t dim) {
    if (!tensor || dim >= tensor->shape.size()) return 0;
    return tensor->shape[dim];
}

LUX_API size_t lux_tensor_numel(lux_tensor tensor) {
    if (!tensor) return 0;
    size_t n = 1;
    for (auto d : tensor->shape) n *= d;
    return n;
}

LUX_API size_t lux_tensor_bytes(lux_tensor tensor) {
    return tensor ? lux_tensor_numel(tensor) * dtype_size(tensor->dtype) : 0;
}

LUX_API lux_dtype lux_tensor_dtype(lux_tensor tensor) {
    return tensor ? tensor->dtype : LUX_DTYPE_F32;
}

LUX_API lux_status lux_tensor_to_host(lux_tensor tensor, void* dst, size_t dst_bytes) {
    if (!tensor || !dst) return LUX_INVALID_ARGUMENT;

    size_t bytes = lux_tensor_bytes(tensor);
    if (dst_bytes < bytes) return LUX_INVALID_ARGUMENT;

    auto mapped = tensor->buffer->map();
    if (!mapped) {
        set_error(mapped.error().message.c_str());
        return to_status(mapped.error());
    }

    std::memcpy(dst, *mapped, bytes);
    tensor->buffer->unmap();
    return LUX_OK;
}

LUX_API lux_status lux_tensor_from_host(lux_tensor tensor, const void* src, size_t src_bytes) {
    if (!tensor || !src) return LUX_INVALID_ARGUMENT;

    size_t bytes = lux_tensor_bytes(tensor);
    if (src_bytes != bytes) return LUX_INVALID_ARGUMENT;

    auto mapped = tensor->buffer->map();
    if (!mapped) {
        set_error(mapped.error().message.c_str());
        return to_status(mapped.error());
    }

    std::memcpy(*mapped, src, bytes);
    tensor->buffer->unmap();
    return LUX_OK;
}

// =============================================================================
// Stub implementations for ops (TODO: implement with real kernels)
// =============================================================================

LUX_API lux_status lux_matmul(lux_session session, lux_tensor a, lux_tensor b, lux_tensor c) {
    (void)session; (void)a; (void)b; (void)c;
    set_error("matmul not yet implemented");
    return LUX_NOT_SUPPORTED;
}

LUX_API lux_status lux_relu(lux_session session, lux_tensor input, lux_tensor output) {
    (void)session; (void)input; (void)output;
    set_error("relu not yet implemented");
    return LUX_NOT_SUPPORTED;
}

LUX_API lux_status lux_gelu(lux_session session, lux_tensor input, lux_tensor output) {
    (void)session; (void)input; (void)output;
    set_error("gelu not yet implemented");
    return LUX_NOT_SUPPORTED;
}

LUX_API lux_status lux_softmax(lux_session session, lux_tensor input, lux_tensor output, int axis) {
    (void)session; (void)input; (void)output; (void)axis;
    set_error("softmax not yet implemented");
    return LUX_NOT_SUPPORTED;
}

LUX_API lux_status lux_layer_norm(lux_session session, lux_tensor input,
                                   lux_tensor gamma, lux_tensor beta,
                                   lux_tensor output, float eps) {
    (void)session; (void)input; (void)gamma; (void)beta; (void)output; (void)eps;
    set_error("layer_norm not yet implemented");
    return LUX_NOT_SUPPORTED;
}

LUX_API lux_status lux_attention(lux_session session, lux_tensor q, lux_tensor k, lux_tensor v,
                                  lux_tensor output, float scale) {
    (void)session; (void)q; (void)k; (void)v; (void)output; (void)scale;
    set_error("attention not yet implemented");
    return LUX_NOT_SUPPORTED;
}

LUX_API lux_status lux_sha256(lux_session session, lux_tensor input, lux_tensor output) {
    (void)session; (void)input; (void)output;
    set_error("sha256 not yet implemented");
    return LUX_NOT_SUPPORTED;
}

LUX_API lux_status lux_keccak256(lux_session session, lux_tensor input, lux_tensor output) {
    (void)session; (void)input; (void)output;
    set_error("keccak256 not yet implemented");
    return LUX_NOT_SUPPORTED;
}

LUX_API lux_status lux_poseidon(lux_session session, lux_tensor input, lux_tensor output) {
    (void)session; (void)input; (void)output;
    set_error("poseidon not yet implemented");
    return LUX_NOT_SUPPORTED;
}

LUX_API lux_status lux_ecdsa_verify_batch(lux_session session, lux_tensor messages,
                                           lux_tensor signatures, lux_tensor pubkeys,
                                           lux_tensor results) {
    (void)session; (void)messages; (void)signatures; (void)pubkeys; (void)results;
    set_error("ecdsa_verify_batch not yet implemented");
    return LUX_NOT_SUPPORTED;
}

LUX_API lux_status lux_ed25519_verify_batch(lux_session session, lux_tensor messages,
                                             lux_tensor signatures, lux_tensor pubkeys,
                                             lux_tensor results) {
    (void)session; (void)messages; (void)signatures; (void)pubkeys; (void)results;
    set_error("ed25519_verify_batch not yet implemented");
    return LUX_NOT_SUPPORTED;
}

LUX_API lux_status lux_bls_verify_batch(lux_session session, lux_tensor messages,
                                         lux_tensor signatures, lux_tensor pubkeys,
                                         lux_tensor results) {
    (void)session; (void)messages; (void)signatures; (void)pubkeys; (void)results;
    set_error("bls_verify_batch not yet implemented");
    return LUX_NOT_SUPPORTED;
}

LUX_API lux_status lux_merkle_root(lux_session session, lux_tensor leaves, lux_tensor root) {
    (void)session; (void)leaves; (void)root;
    set_error("merkle_root not yet implemented");
    return LUX_NOT_SUPPORTED;
}

LUX_API lux_status lux_ntt(lux_session session, lux_tensor input, lux_tensor output,
                            lux_tensor roots, uint64_t modulus) {
    (void)session; (void)input; (void)output; (void)roots; (void)modulus;
    set_error("ntt not yet implemented");
    return LUX_NOT_SUPPORTED;
}

LUX_API lux_status lux_intt(lux_session session, lux_tensor input, lux_tensor output,
                             lux_tensor inv_roots, uint64_t modulus) {
    (void)session; (void)input; (void)output; (void)inv_roots; (void)modulus;
    set_error("intt not yet implemented");
    return LUX_NOT_SUPPORTED;
}

LUX_API lux_status lux_msm(lux_session session, lux_tensor scalars, lux_tensor bases,
                            lux_tensor result) {
    (void)session; (void)scalars; (void)bases; (void)result;
    set_error("msm not yet implemented");
    return LUX_NOT_SUPPORTED;
}

LUX_API lux_status lux_poly_mul(lux_session session, lux_tensor a, lux_tensor b,
                                 lux_tensor c, uint64_t modulus) {
    (void)session; (void)a; (void)b; (void)c; (void)modulus;
    set_error("poly_mul not yet implemented");
    return LUX_NOT_SUPPORTED;
}

LUX_API lux_status lux_kyber_keygen(lux_session session, lux_tensor pk, lux_tensor sk) {
    (void)session; (void)pk; (void)sk;
    set_error("kyber_keygen not yet implemented");
    return LUX_NOT_SUPPORTED;
}

LUX_API lux_status lux_kyber_encaps(lux_session session, lux_tensor pk,
                                     lux_tensor ct, lux_tensor ss) {
    (void)session; (void)pk; (void)ct; (void)ss;
    set_error("kyber_encaps not yet implemented");
    return LUX_NOT_SUPPORTED;
}

LUX_API lux_status lux_kyber_decaps(lux_session session, lux_tensor ct,
                                     lux_tensor sk, lux_tensor ss) {
    (void)session; (void)ct; (void)sk; (void)ss;
    set_error("kyber_decaps not yet implemented");
    return LUX_NOT_SUPPORTED;
}

LUX_API lux_status lux_dilithium_sign(lux_session session, lux_tensor msg,
                                       lux_tensor sk, lux_tensor sig) {
    (void)session; (void)msg; (void)sk; (void)sig;
    set_error("dilithium_sign not yet implemented");
    return LUX_NOT_SUPPORTED;
}

LUX_API lux_status lux_dilithium_verify(lux_session session, lux_tensor msg,
                                         lux_tensor sig, lux_tensor pk, int* valid) {
    (void)session; (void)msg; (void)sig; (void)pk; (void)valid;
    set_error("dilithium_verify not yet implemented");
    return LUX_NOT_SUPPORTED;
}

LUX_API lux_status lux_bfv_encrypt(lux_session session, lux_tensor plaintext,
                                    lux_tensor pk, lux_tensor ciphertext) {
    (void)session; (void)plaintext; (void)pk; (void)ciphertext;
    set_error("bfv_encrypt not yet implemented");
    return LUX_NOT_SUPPORTED;
}

LUX_API lux_status lux_bfv_decrypt(lux_session session, lux_tensor ciphertext,
                                    lux_tensor sk, lux_tensor plaintext) {
    (void)session; (void)ciphertext; (void)sk; (void)plaintext;
    set_error("bfv_decrypt not yet implemented");
    return LUX_NOT_SUPPORTED;
}

LUX_API lux_status lux_bfv_add(lux_session session, lux_tensor ct1,
                                lux_tensor ct2, lux_tensor result) {
    (void)session; (void)ct1; (void)ct2; (void)result;
    set_error("bfv_add not yet implemented");
    return LUX_NOT_SUPPORTED;
}

LUX_API lux_status lux_bfv_multiply(lux_session session, lux_tensor ct1,
                                     lux_tensor ct2, lux_tensor relin_key,
                                     lux_tensor result) {
    (void)session; (void)ct1; (void)ct2; (void)relin_key; (void)result;
    set_error("bfv_multiply not yet implemented");
    return LUX_NOT_SUPPORTED;
}

LUX_API lux_status lux_constant_product_swap(lux_session session,
                                              lux_tensor reserve_x, lux_tensor reserve_y,
                                              lux_tensor amount_in, int x_to_y,
                                              lux_tensor amount_out, float fee) {
    (void)session; (void)reserve_x; (void)reserve_y; (void)amount_in;
    (void)x_to_y; (void)amount_out; (void)fee;
    set_error("constant_product_swap not yet implemented");
    return LUX_NOT_SUPPORTED;
}

LUX_API lux_status lux_compute_twap(lux_session session, lux_tensor prices,
                                     lux_tensor timestamps, uint64_t start, uint64_t end,
                                     lux_tensor twap) {
    (void)session; (void)prices; (void)timestamps; (void)start; (void)end; (void)twap;
    set_error("compute_twap not yet implemented");
    return LUX_NOT_SUPPORTED;
}

LUX_API lux_status lux_match_orders(lux_session session, lux_tensor bids, lux_tensor asks,
                                     lux_tensor matches, lux_tensor prices, lux_tensor amounts) {
    (void)session; (void)bids; (void)asks; (void)matches; (void)prices; (void)amounts;
    set_error("match_orders not yet implemented");
    return LUX_NOT_SUPPORTED;
}

} // extern "C"
