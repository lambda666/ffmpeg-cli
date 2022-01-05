#ifndef _FFMPEG_LEGACY_API_H__
#define _FFMPEG_LEGACY_API_H__

#ifdef __cplusplus
extern "C"
{
#endif

#if defined _WIN32
#   define _FFMPEG_API __declspec(dllexport)
#elif defined __GNUC__ && __GNUC__ >= 4
#   define _FFMPEG_API __attribute__ ((visibility ("default")))
#else
#   define _FFMPEG_API
#endif

typedef struct FF_VideoDecoder FF_VideoDecoder;
typedef struct FF_VideoEncoder FF_VideoEncoder;
///////////////////////////////////////////////////////////////////////////////////////////////////
_FFMPEG_API FF_VideoDecoder* FF_VideoDecoder_Create( const char* filename);
_FFMPEG_API int FF_VideoDecoder_SetProperty(struct FF_VideoDecoder* cap,
                                                  int prop, double value);
_FFMPEG_API double FF_VideoDecoder_GetProperty(struct FF_VideoDecoder* cap, int prop);
_FFMPEG_API int FF_VideoDecoder_GrabFrame(struct FF_VideoDecoder* cap);
_FFMPEG_API int FF_VideoDecoder_RetrieveFrame(struct FF_VideoDecoder* capture, unsigned char** data,
                                             int* step, int* width, int* height, int* cn);
_FFMPEG_API void FF_VideoDecoder_Release(struct FF_VideoDecoder** cap);
///////////////////////////////////////////////////////////////////////////////////////////////////
_FFMPEG_API struct FF_VideoEncoder* FF_VideoEncoder_Create(const char* filename,
            int fourcc, double fps, int width, int height, int isColor );
_FFMPEG_API int FF_VideoEncoder_WriteFrame(struct FF_VideoEncoder* writer, const unsigned char* data,
                                          int step, int width, int height, int cn, int origin);
_FFMPEG_API void FF_VideoEncoder_Release(struct FF_VideoEncoder** writer);
///////////////////////////////////////////////////////////////////////////////////////////////////
#ifdef __cplusplus
}
#endif

#endif  // _FFMPEG_LEGACY_API_H__
