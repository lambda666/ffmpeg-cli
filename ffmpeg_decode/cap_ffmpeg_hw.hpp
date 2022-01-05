// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.
//
// Copyright (C) 2020-2021 Intel Corporation

#include <sstream>


// FFMPEG "C" headers
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
}

enum VideoAccelerationType
{
    VIDEO_ACCELERATION_NONE     =  0,  //!< Do not require any specific H/W acceleration, prefer software processing.
                                       //!< Reading of this value means that special H/W accelerated handling is not added or not detected by OpenCV.
    VIDEO_ACCELERATION_ANY      =  1,  //!< Prefer to use H/W acceleration. If no one supported, then fallback to software processing.
                                       //!< @note H/W acceleration may require special configuration of used environment.
                                       //!< @note Results in encoding scenario may differ between software and hardware accelerated encoders.
    VIDEO_ACCELERATION_D3D11    =  2,  //!< DirectX 11
    VIDEO_ACCELERATION_VAAPI    =  3,  //!< VAAPI
    VIDEO_ACCELERATION_MFX      =  4,  //!< libmfx (Intel MediaSDK/oneVPL)
};

#define HW_DEFAULT_POOL_SIZE    32
#define HW_DEFAULT_SW_FORMAT    AV_PIX_FMT_NV12

using namespace cv;

static AVCodec *hw_find_codec(AVCodecID id, AVHWDeviceType hw_type, int (*check_category)(const AVCodec *),
                              const char *disabled_codecs, AVPixelFormat *hw_pix_fmt);
static AVBufferRef* hw_create_device(AVHWDeviceType hw_type, int hw_device, const std::string& device_subname, bool use_opencl);
static AVBufferRef* hw_create_frames(struct AVCodecContext* ctx, AVBufferRef *hw_device_ctx, int width, int height, AVPixelFormat hw_format);
static AVPixelFormat hw_get_format_callback(struct AVCodecContext *ctx, const enum AVPixelFormat * fmt);
static VideoAccelerationType hw_type_to_va_type(AVHWDeviceType hw_type);

static
const char* getVideoAccelerationName(VideoAccelerationType va_type)
{
    switch (va_type)
    {
    case VIDEO_ACCELERATION_NONE: return "none";
    case VIDEO_ACCELERATION_ANY: return "any";
    case VIDEO_ACCELERATION_D3D11: return "d3d11";
    case VIDEO_ACCELERATION_VAAPI: return "vaapi";
    case VIDEO_ACCELERATION_MFX: return "mfx";
    }
    return "unknown";
}

static
std::string getDecoderConfiguration(VideoAccelerationType va_type, AVDictionary *dict)
{
    std::string va_name = getVideoAccelerationName(va_type);
    std::string key_name = std::string("hw_decoders_") + va_name;
    const char *hw_acceleration = NULL;
    if (dict)
    {
        AVDictionaryEntry* entry = av_dict_get(dict, key_name.c_str(), NULL, 0);
        if (entry)
            hw_acceleration = entry->value;
    }
    if (hw_acceleration)
        return hw_acceleration;

    // some default values (FFMPEG_DECODE_ACCELERATION_TYPES)
#ifdef _WIN32
    switch (va_type)
    {
    case VIDEO_ACCELERATION_NONE: return "";
    case VIDEO_ACCELERATION_ANY: return "d3d11va";
    case VIDEO_ACCELERATION_D3D11: return "d3d11va";
    case VIDEO_ACCELERATION_VAAPI: return "";
    case VIDEO_ACCELERATION_MFX: return ""; // "qsv" fails if non-Intel D3D11 device
    }
    return "";
#else
    switch (va_type)
    {
    case VIDEO_ACCELERATION_NONE: return "";
    case VIDEO_ACCELERATION_ANY: return "vaapi.iHD";
    case VIDEO_ACCELERATION_D3D11: return "";
    case VIDEO_ACCELERATION_VAAPI: return "vaapi.iHD";
    case VIDEO_ACCELERATION_MFX: return "qsv.iHD";
    }
    return "";
#endif
}

static
std::string getEncoderConfiguration(VideoAccelerationType va_type, AVDictionary *dict)
{
    std::string va_name = getVideoAccelerationName(va_type);
    std::string key_name = std::string("hw_encoders_") + va_name;
    const char *hw_acceleration = NULL;
    if (dict)
    {
        AVDictionaryEntry* entry = av_dict_get(dict, key_name.c_str(), NULL, 0);
        if (entry)
            hw_acceleration = entry->value;
    }
    if (hw_acceleration)
        return hw_acceleration;

    // some default values (FFMPEG_ENCODE_ACCELERATION_TYPES)
#ifdef _WIN32
    switch (va_type)
    {
    case VIDEO_ACCELERATION_NONE: return "";
    case VIDEO_ACCELERATION_ANY: return "qsv";
    case VIDEO_ACCELERATION_D3D11: return "";
    case VIDEO_ACCELERATION_VAAPI: return "";
    case VIDEO_ACCELERATION_MFX: return "qsv";
    }
    return "";
#else
    switch (va_type)
    {
    case VIDEO_ACCELERATION_NONE: return "";
    case VIDEO_ACCELERATION_ANY: return "qsv.iHD,vaapi.iHD";
    case VIDEO_ACCELERATION_D3D11: return "";
    case VIDEO_ACCELERATION_VAAPI: return "vaapi.iHD";
    case VIDEO_ACCELERATION_MFX: return "qsv.iHD";
    }
    return "unknown";
#endif
}

static
std::string getDecoderDisabledCodecs(AVDictionary *dict)
{
    std::string key_name = std::string("hw_disable_decoders");
    const char *disabled_codecs = NULL;
    if (dict)
    {
        AVDictionaryEntry* entry = av_dict_get(dict, key_name.c_str(), NULL, 0);
        if (entry)
            disabled_codecs = entry->value;
    }
    if (disabled_codecs)
        return disabled_codecs;

    // some default values (FFMPEG_DECODE_DISABLE_CODECS)
#ifdef _WIN32
    return "none";
#else
    return "av1.vaapi,av1_qsv,vp8.vaapi,vp8_qsv";  // "vp9_qsv"
#endif
}

static
std::string getEncoderDisabledCodecs(AVDictionary *dict)
{
    std::string key_name = std::string("hw_disabled_encoders");
    const char *disabled_codecs = NULL;
    if (dict)
    {
        AVDictionaryEntry* entry = av_dict_get(dict, key_name.c_str(), NULL, 0);
        if (entry)
            disabled_codecs = entry->value;
    }
    if (disabled_codecs)
        return disabled_codecs;

    // some default values (FFMPEG_ENCODE_DISABLE_CODECS)
#ifdef _WIN32
    return "mjpeg_qsv";
#else
    return "mjpeg_vaapi,mjpeg_qsv,vp8_vaapi";
#endif
}

static
bool hw_check_device(AVBufferRef* ctx, AVHWDeviceType hw_type, const std::string& device_subname) {
    if (!ctx)
        return false;
    AVHWDeviceContext* hw_device_ctx = (AVHWDeviceContext*)ctx->data;
    if (!hw_device_ctx->hwctx)
        return false;
    const char *hw_name = av_hwdevice_get_type_name(hw_type);
    if (hw_type == AV_HWDEVICE_TYPE_QSV)
        hw_name = "MFX";
    bool ret = true;
    std::string device_name;
    
    if (hw_device_ctx->type == AV_HWDEVICE_TYPE_VAAPI) {
        ret = (hw_type != AV_HWDEVICE_TYPE_QSV); // disable MFX if we can't check VAAPI for VideoProc entrypoint
    }
    if (ret && !device_subname.empty() && device_name.find(device_subname) == std::string::npos)
    {
        // CV_LOG_INFO(NULL, "FFMPEG: Skipping '" << hw_name <<
        //     "' video acceleration on the following device name as not matching substring '" << device_subname << "': " << device_name);
        ret = false;  // reject configuration
    }
    if (ret)
    {
        if (!device_name.empty()) {
            // CV_LOG_INFO(NULL, "FFMPEG: Using " << hw_name << " video acceleration on device: " << device_name);
        } else {
            // CV_LOG_INFO(NULL, "FFMPEG: Using " << hw_name << " video acceleration");
        }
    }
    return ret;
}

static
AVBufferRef* hw_create_derived_context(AVHWDeviceType hw_type, AVBufferRef* hw_device_ctx) {
    AVBufferRef* derived_ctx = NULL;
    const char* hw_name = av_hwdevice_get_type_name(hw_type);
    int err = av_hwdevice_ctx_create_derived(&derived_ctx, hw_type, hw_device_ctx, 0);
    if (!derived_ctx || err < 0)
    {
        if (derived_ctx)
            av_buffer_unref(&derived_ctx);
        // CV_LOG_INFO(NULL, "FFMPEG: Failed to create derived video acceleration (av_hwdevice_ctx_create_derived) for " << hw_name << ". Error=" << err);
        return NULL;
    }
    else
    {
        // Store child context in 'user_opaque' field of parent context.
        struct FreeChildContext {
            static void free(struct AVHWDeviceContext* ctx) {
                AVBufferRef* child_ctx = (AVBufferRef*)ctx->user_opaque;
                if (child_ctx)
                    av_buffer_unref(&child_ctx);
            }
        };
        AVHWDeviceContext* ctx = (AVHWDeviceContext*)derived_ctx->data;
        ctx->user_opaque = av_buffer_ref(hw_device_ctx);
        ctx->free = FreeChildContext::free;
        // CV_LOG_NFO(NULL, "FFMPEG: Created derived video acceleration context (av_hwdevice_ctx_create_derived) for " << hw_name);
        return derived_ctx;
    }
}


static
AVBufferRef* hw_create_device(AVHWDeviceType hw_type, int hw_device, const std::string& device_subname, bool use_opencl) {
    AVBufferRef* hw_device_ctx = NULL;
    if (AV_HWDEVICE_TYPE_NONE == hw_type)
        return NULL;

    // Create new media context. In QSV case, first create 'child' context.
    std::vector<AVHWDeviceType> child_types = { hw_type };
    if (hw_type == AV_HWDEVICE_TYPE_QSV) {
#ifdef _WIN32
        child_types = { AV_HWDEVICE_TYPE_D3D11VA, AV_HWDEVICE_TYPE_DXVA2 };
#else
        child_types = { AV_HWDEVICE_TYPE_VAAPI };
#endif
    }
    for (AVHWDeviceType child_type : child_types) {
        char device[128] = "";
        char* pdevice = NULL;
        if (hw_device >= 0 && hw_device < 100000) {
            if (child_type == AV_HWDEVICE_TYPE_VAAPI) {
                snprintf(device, sizeof(device), "/dev/dri/renderD%d", 128 + hw_device);
            }
            else {
                snprintf(device, sizeof(device), "%d", hw_device);
            }
            pdevice = device;
        }
        const char* hw_child_name = av_hwdevice_get_type_name(child_type);
        const char* device_name = pdevice ? pdevice : "'default'";
        int err = av_hwdevice_ctx_create(&hw_device_ctx, child_type, pdevice, NULL, 0);
        if (hw_device_ctx && err >= 0)
        {
            if (!hw_check_device(hw_device_ctx, hw_type, device_subname)) {
                av_buffer_unref(&hw_device_ctx);
                continue;
            }
            // CV_LOG_INFO(NULL, "FFMPEG: Created video acceleration context (av_hwdevice_ctx_create) for " << hw_child_name << " on device " << device_name);
            _UNUSED(use_opencl);
            if (hw_type != child_type) {
                AVBufferRef* derived_ctx = hw_create_derived_context(hw_type, hw_device_ctx);
                av_buffer_unref(&hw_device_ctx);
                return derived_ctx;
            } else {
                return hw_device_ctx;
            }
        }
        else
        {
            const char* hw_name = hw_child_name;
            // CV_LOG_INFO(NULL, "FFMPEG: Failed to create " << hw_name << " video acceleration (av_hwdevice_ctx_create) on device " << device_name);
        }
    }
    return NULL;
}

static
AVBufferRef* hw_create_frames(struct AVCodecContext* codec_ctx, AVBufferRef *hw_device_ctx, int width, int height, AVPixelFormat hw_format)
{
    AVHWDeviceContext *device_ctx = (AVHWDeviceContext*)hw_device_ctx->data;
    AVBufferRef* child_ctx = hw_device_ctx;
    // In QSV case we first allocate child D3D11/VAAPI frames (except DXVA2 as no OpenCL interop), then derive to parent QSV frames
    if (AV_HWDEVICE_TYPE_QSV == device_ctx->type) {
        AVBufferRef *ctx = (AVBufferRef *) device_ctx->user_opaque; // child context stored during creation of derived context
        if (ctx && AV_HWDEVICE_TYPE_DXVA2 != ((AVHWDeviceContext *) ctx->data)->type) {
            child_ctx = ctx;
        }
    }
    AVBufferRef *hw_frames_ref = nullptr;
    if (codec_ctx)
    {
        int res = avcodec_get_hw_frames_parameters(codec_ctx, child_ctx, hw_format, &hw_frames_ref);
        if (res < 0)
        {
            // CV_LOG_DEBUG(NULL, "FFMPEG: avcodec_get_hw_frames_parameters() call failed: " << res)
        }
    }
    if (!hw_frames_ref)
    {
        hw_frames_ref = av_hwframe_ctx_alloc(child_ctx);
    }
    if (!hw_frames_ref)
    {
        // CV_LOG_INFO(NULL, "FFMPEG: Failed to create HW frame context (av_hwframe_ctx_alloc)");
        return NULL;
    }
    AVHWFramesContext *frames_ctx = (AVHWFramesContext *)(hw_frames_ref->data);
    frames_ctx->width = width;
    frames_ctx->height = height;
    if (frames_ctx->format == AV_PIX_FMT_NONE) {
        if (child_ctx == hw_device_ctx) {
            frames_ctx->format = hw_format;
        }
        else {
            AVHWFramesConstraints* constraints = av_hwdevice_get_hwframe_constraints(child_ctx, NULL);
            if (constraints) {
                frames_ctx->format = constraints->valid_hw_formats[0];
                av_hwframe_constraints_free(&constraints);
            }
        }
    }
    if (frames_ctx->sw_format == AV_PIX_FMT_NONE)
        frames_ctx->sw_format = HW_DEFAULT_SW_FORMAT;
    if (frames_ctx->initial_pool_size == 0)
        frames_ctx->initial_pool_size = HW_DEFAULT_POOL_SIZE;

    int res = av_hwframe_ctx_init(hw_frames_ref);
    if (res < 0)
    {
        // CV_LOG_INFO(NULL, "FFMPEG: Failed to initialize HW frame context (av_hwframe_ctx_init): " << res);
        av_buffer_unref(&hw_frames_ref);
        return NULL;
    }

    if (child_ctx != hw_device_ctx) {
        AVBufferRef* derived_frame_ctx = NULL;
        int flags = AV_HWFRAME_MAP_READ | AV_HWFRAME_MAP_WRITE;
        res = av_hwframe_ctx_create_derived(&derived_frame_ctx, hw_format, hw_device_ctx, hw_frames_ref, flags);
        av_buffer_unref(&hw_frames_ref);
        if (res < 0)
        {
            // CV_LOG_INFO(NULL, "FFMPEG: Failed to create derived HW frame context (av_hwframe_ctx_create_derived): " << res);
            return NULL;
        }
        else {
            ((AVHWFramesContext*)derived_frame_ctx->data)->user_opaque = frames_ctx;
            return derived_frame_ctx;
        }
    }
    else {
        return hw_frames_ref;
    }
}

static
bool hw_check_codec(AVCodec* codec, AVHWDeviceType hw_type, const char *disabled_codecs)
{
    CV_Assert(disabled_codecs);
    std::string hw_name = std::string(".") + av_hwdevice_get_type_name(hw_type);
    std::stringstream s_stream(disabled_codecs);
    while (s_stream.good()) {
        std::string name;
        getline(s_stream, name, ',');
        if (name == codec->name || name == hw_name || name == codec->name + hw_name || name == "hw") {
            // CV_LOG_INFO(NULL, "FFMPEG: skipping codec " << codec->name << hw_name);
            return false;
        }
    }
    return true;
}

static
AVCodec *hw_find_codec(AVCodecID id, AVHWDeviceType hw_type, int (*check_category)(const AVCodec *), const char *disabled_codecs, AVPixelFormat *hw_pix_fmt) {
    AVCodec *c = 0;
    void *opaque = 0;

    while (NULL != (c = (AVCodec*)av_codec_iterate(&opaque)))
    {
        if (!check_category(c))
            continue;
        if (c->id != id)
            continue;
        if (c->capabilities & AV_CODEC_CAP_EXPERIMENTAL)
            continue;
        if (hw_type != AV_HWDEVICE_TYPE_NONE) {
            AVPixelFormat hw_native_fmt = AV_PIX_FMT_NONE;
#if LIBAVUTIL_BUILD < AV_VERSION_INT(56, 51, 100) // VAAPI encoders support avcodec_get_hw_config() starting ffmpeg 4.3
            if (hw_type == AV_HWDEVICE_TYPE_VAAPI)
                hw_native_fmt = AV_PIX_FMT_VAAPI_VLD;
#endif
            if (hw_type == AV_HWDEVICE_TYPE_CUDA) // CUDA encoders don't support avcodec_get_hw_config()
                hw_native_fmt = AV_PIX_FMT_CUDA;
            if (av_codec_is_encoder(c) && hw_native_fmt != AV_PIX_FMT_NONE && c->pix_fmts) {
                for (int i = 0; c->pix_fmts[i] != AV_PIX_FMT_NONE; i++) {
                    if (c->pix_fmts[i] == hw_native_fmt) {
                        *hw_pix_fmt = hw_native_fmt;
                        if (hw_check_codec(c, hw_type, disabled_codecs))
                            return c;
                    }
                }
            }
            for (int i = 0;; i++) {
                const AVCodecHWConfig *hw_config = avcodec_get_hw_config(c, i);
                if (!hw_config)
                    break;
                if (hw_config->device_type == hw_type) {
                    *hw_pix_fmt = hw_config->pix_fmt;
                    if (hw_check_codec(c, hw_type, disabled_codecs))
                        return c;
                }
            }
        } else {
            return c;
        }
    }

    return NULL;
}

// Callback to select hardware pixel format (not software format) and allocate frame pool (hw_frames_ctx)
static
AVPixelFormat hw_get_format_callback(struct AVCodecContext *ctx, const enum AVPixelFormat * fmt) {
    if (!ctx->hw_device_ctx)
        return fmt[0];
    AVHWDeviceType hw_type = ((AVHWDeviceContext*)ctx->hw_device_ctx->data)->type;
    for (int j = 0;; j++) {
        const AVCodecHWConfig *hw_config = avcodec_get_hw_config(ctx->codec, j);
        if (!hw_config)
            break;
        if (hw_config->device_type == hw_type) {
            for (int i = 0; fmt[i] != AV_PIX_FMT_NONE; i++) {
                if (fmt[i] == hw_config->pix_fmt) {
                    if (hw_config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_FRAMES_CTX) {
                        ctx->sw_pix_fmt = HW_DEFAULT_SW_FORMAT;
                        ctx->hw_frames_ctx = hw_create_frames(ctx, ctx->hw_device_ctx, ctx->width, ctx->height, fmt[i]);
                        if (ctx->hw_frames_ctx) {
                            //ctx->sw_pix_fmt = ((AVHWFramesContext *)(ctx->hw_frames_ctx->data))->sw_format;
                            return fmt[i];
                        }
                    }
                }
            }
        }
    }
    // CV_LOG_DEBUG(NULL, "FFMPEG: Can't select HW format in 'get_format()' callback, use default");
    return fmt[0];
}

// GPU color conversion NV12->BGRA via OpenCL extensions
// static bool
// hw_copy_frame_to_umat(AVBufferRef* ctx, AVFrame* hw_frame, cv::OutputArray output) {
//     _UNUSED(hw_frame);
//     _UNUSED(output);
//     if (!ctx)
//         return false;
//     return false;
// }

// GPU color conversion BGRA->NV12 via OpenCL extensions
// static bool
// hw_copy_umat_to_frame(AVBufferRef* ctx, cv::InputArray input, AVFrame* hw_frame) {
//     _UNUSED(input);
//     _UNUSED(hw_frame);
//     if (!ctx)
//         return false;
//     return false;
// }

static
VideoAccelerationType hw_type_to_va_type(AVHWDeviceType hw_type) {
    struct HWTypeFFMPEG {
        AVHWDeviceType hw_type;
        VideoAccelerationType va_type;
    } known_hw_types[] = {
            { AV_HWDEVICE_TYPE_D3D11VA, VIDEO_ACCELERATION_D3D11 },
            { AV_HWDEVICE_TYPE_VAAPI, VIDEO_ACCELERATION_VAAPI },
            { AV_HWDEVICE_TYPE_QSV, VIDEO_ACCELERATION_MFX },
            { AV_HWDEVICE_TYPE_CUDA, (VideoAccelerationType)(1 << 11) },
    };
    for (const HWTypeFFMPEG& hw : known_hw_types) {
        if (hw_type == hw.hw_type)
            return hw.va_type;
    }
    return VIDEO_ACCELERATION_NONE;
}

class HWAccelIterator {
public:
    HWAccelIterator(VideoAccelerationType va_type, bool isEncoder, AVDictionary *dict)
        : hw_type_(AV_HWDEVICE_TYPE_NONE)
    {
        std::string accel_list;
        if (va_type != VIDEO_ACCELERATION_NONE)
        {
            updateAccelList_(accel_list, va_type, isEncoder, dict);
        }
        if (va_type == VIDEO_ACCELERATION_ANY)
        {
            if (!accel_list.empty())
                accel_list += ",";  // add no-acceleration case to the end of the list
        }
        // CV_LOG_DEBUG(NULL, "FFMPEG: allowed acceleration types (" << getVideoAccelerationName(va_type) << "): '" << accel_list << "'");

        if (accel_list.empty() && va_type != VIDEO_ACCELERATION_NONE && va_type != VIDEO_ACCELERATION_ANY)
        {
            // broke stream
            std::string tmp;
            s_stream_ >> tmp;
        }
        else
        {
            s_stream_ = std::istringstream(accel_list);
        }

        if (va_type != VIDEO_ACCELERATION_NONE)
        {
            disabled_codecs_ = isEncoder
                    ? getEncoderDisabledCodecs(dict)
                    : getDecoderDisabledCodecs(dict);
            // CV_LOG_DEBUG(NULL, "FFMPEG: disabled codecs: '" << disabled_codecs_ << "'");
        }
    }
    bool good() const
    {
        return s_stream_.good();
    }
    void parse_next()
    {
        getline(s_stream_, hw_type_device_string_, ',');
        size_t index = hw_type_device_string_.find('.');
        if (index != std::string::npos) {
            device_subname_ = hw_type_device_string_.substr(index + 1);
            hw_type_string_ = hw_type_device_string_.substr(0, index);
        } else {
            device_subname_.clear();
            hw_type_string_ = hw_type_device_string_;
        }
        hw_type_ = av_hwdevice_find_type_by_name(hw_type_string_.c_str());
    }
    const std::string& hw_type_device_string() const { return hw_type_device_string_; }
    const std::string& hw_type_string() const { return hw_type_string_; }
    AVHWDeviceType hw_type() const { return hw_type_; }
    const std::string& device_subname() const { return device_subname_; }
    const std::string& disabled_codecs() const { return disabled_codecs_; }
private:
    bool updateAccelList_(std::string& accel_list, VideoAccelerationType va_type, bool isEncoder, AVDictionary *dict)
    {
        std::string new_accels = isEncoder
                ? getEncoderConfiguration(va_type, dict)
                : getDecoderConfiguration(va_type, dict);
        if (new_accels.empty())
            return false;
        if (accel_list.empty())
            accel_list = new_accels;
        else
            accel_list = accel_list + "," + new_accels;
        return true;
    }
    std::istringstream s_stream_;
    std::string hw_type_device_string_;
    std::string hw_type_string_;
    AVHWDeviceType hw_type_;
    std::string device_subname_;

    std::string disabled_codecs_;
};
