/**
 * Copyright Off World Live Ltd (https://offworld.live), 2019-2021
 *
 * and licenced under the GPL v2 (https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html)
 *
 * Many thanks to authors of https://github.com/baffler/OBS-OpenVR-Input-Plugin which
 * was used as guidance to working with the OBS Studio APIs
 */
#include <cstdint>
#include <string>
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/threading.h>
#include <media-io/video-frame.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include <pipewire/pipewire.h>
#include <pipewire/stream.h>
#include <graphics/graphics.h>
#include <drm_fourcc.h>
#include <spa/param/video/raw-utils.h>

PFNEGLEXPORTDMABUFIMAGEMESAPROC eglExportDMABUFImageMESA =
    reinterpret_cast<PFNEGLEXPORTDMABUFIMAGEMESAPROC>(
        eglGetProcAddress("eglExportDMABUFImageMESA"));
PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC eglExportDMABUFImageQueryMESA =
	reinterpret_cast<PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC>(
		eglGetProcAddress("eglExportDMABUFImageQueryMESA"));
#define FILTER_PROP_NAME "PW_filter_name"

struct pw_filter {
	// mutex guards accesses to fields in SHARED section
	// and any methods on spoutDX* filter_sender.
	// Calling obs methods on obs types seems thread-safe.
	// trying to avoid calling obs methods while holding our own mutex.
	pthread_mutex_t mutex;

	// [SHARED]
	obs_source_t *source_context;
	const char *sender_name; // owned by obs
	//sender_name belongs to obs and obs is able to change the pointer, which is not good for determining if the name has been changed or not, so we need to keep a copy for comparing
	std::string ssender_name;

	//this whole thing is for managing textures, texture export and pipewire shenanigans
	int fd = -1;
	GLuint tex_handle;
	EGLImage image;
	EGLDisplay display;
	EGLContext egl_context;
	EGLint stride = 0;
	EGLint offset = 0;
	EGLuint64KHR modifier = DRM_FORMAT_MOD_INVALID;
	int forcc = -1;
	struct pw_buffer *buffer;
	struct pw_thread_loop *loop;
    struct pw_context *context;
    struct pw_core *core;
    struct pw_stream *stream;
	struct spa_video_info_raw format;

	// [RENDER] After creation, only accessed on render thread
	gs_texrender_t *texrender;		// owned by filter
	gs_stagesurf_t *stagesurface;		// "
	obs_source_t *target_render;
	uint32_t width;
	uint32_t height;
	double fps;

	// set after we successfully init on render thread
	bool is_initialised;
	// detect that source is still active by setting in _videorender() and clearing in _offscreen_render()
	bool is_active;
	// checking on buffer pool
	bool buffer_configured = false;
	// checking the state of pipewire stream
	bool is_streaming = false;
	//only used for the situations that fd is changed and there needs to be a little re-initializations on that
	bool is_changed = false;
};
static spa_video_format drm_to_spa(int forcc){
	switch(forcc){
		case DRM_FORMAT_ABGR8888:
			return SPA_VIDEO_FORMAT_RGBA;
		case DRM_FORMAT_ARGB8888:
			return SPA_VIDEO_FORMAT_BGRA;
		case DRM_FORMAT_BGRA8888:
			return SPA_VIDEO_FORMAT_ARGB;
		case DRM_FORMAT_RGBA8888:
			return SPA_VIDEO_FORMAT_ABGR;
		case DRM_FORMAT_BGR888:
			return SPA_VIDEO_FORMAT_RGB;
		case DRM_FORMAT_RGB888:
			return SPA_VIDEO_FORMAT_BGR;
		case DRM_FORMAT_ARGB2101010:
			return SPA_VIDEO_FORMAT_ARGB_210LE;
        case DRM_FORMAT_ABGR2101010:
			return SPA_VIDEO_FORMAT_ABGR_210LE;
        case DRM_FORMAT_RGBA1010102:
			return SPA_VIDEO_FORMAT_RGBA_102LE;
        case DRM_FORMAT_BGRA1010102:
			return SPA_VIDEO_FORMAT_BGRA_102LE;
        case DRM_FORMAT_ARGB16161616:
			return SPA_VIDEO_FORMAT_ARGB64;
        case DRM_FORMAT_ABGR16161616F:
			return SPA_VIDEO_FORMAT_RGBA_F16;
        case DRM_FORMAT_ABGR32323232F:
			return SPA_VIDEO_FORMAT_RGBA_F32;
		default:
			//just making the compiler shutup
			return SPA_VIDEO_FORMAT_BGRA;
	}
}

const char *pw_filter_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("pipewire export");
}

void pw_filter_getdefaults(obs_data_t *defaults)
{
	obs_data_set_default_string(defaults, FILTER_PROP_NAME, obs_module_text("defaultfiltername"));
}

static void on_stream_state_changed(void *data, enum pw_stream_state old,enum pw_stream_state state, const char *error)
{
	struct pw_filter *context = (pw_filter *)data;

	blog(LOG_INFO, "[pipewire filter] stream state: %s -> %s%s%s",
		pw_stream_state_as_string(old), pw_stream_state_as_string(state),error ? " | error: " : "", error ? error : "");
	switch(state){
		case PW_STREAM_STATE_PAUSED:{
			pw_stream_set_active(context->stream, true);
			break;
		}
		case PW_STREAM_STATE_STREAMING:{
			blog(LOG_INFO, "[pipewire filter] driving: %d", pw_stream_is_driving(context->stream));
			context->is_streaming = true;
			break;
		}
		default:
			context->is_streaming = false;
	}
}

static void try_configure_buffer(void *data)
{
	struct pw_filter *context = (pw_filter *)data;
	if (context->buffer_configured || !context->buffer || context->fd < 0)
		return;

	struct spa_data *spadata = context->buffer->buffer->datas;
	spadata[0].type = SPA_DATA_DmaBuf;
	spadata[0].fd = context->fd;
	spadata[0].flags = SPA_DATA_FLAG_READABLE;
	spadata[0].mapoffset = 0;
	spadata[0].maxsize = context->height * context->stride;
	spadata[0].chunk->offset = context->offset;
	spadata[0].chunk->stride = context->stride;
	spadata[0].chunk->size = context->height * context->stride;
	spadata[0].chunk->flags = SPA_CHUNK_FLAG_NONE;

	context->buffer_configured = true;
	blog(LOG_INFO, "[pipewire filter] pool buffer configured with dmabuf fd %d (stride %d)",
	context->fd, context->stride);
}

static void try_configure_buffer(void *data, pw_buffer* buffer)
{
	struct pw_filter *context = (pw_filter *)data;
	if (context->buffer_configured || !buffer || context->fd < 0)
		return;

	struct spa_data *spadata = buffer->buffer->datas;
	spadata[0].type = SPA_DATA_DmaBuf;
	spadata[0].fd = context->fd;
	spadata[0].flags = SPA_DATA_FLAG_READABLE;
	spadata[0].mapoffset = 0;
	spadata[0].maxsize = context->height * context->stride;
	spadata[0].chunk->offset = context->offset;
	spadata[0].chunk->stride = context->stride;
	spadata[0].chunk->size = context->height * context->stride;
	spadata[0].chunk->flags = SPA_CHUNK_FLAG_NONE;

	context->buffer_configured = true;
	blog(LOG_INFO, "[pipewire filter] pool buffer configured with dmabuf fd %d (stride %d)", context->fd, context->stride);
}

static void on_add_buffer(void *data, struct pw_buffer *buffer)
{
	struct pw_filter *context = (pw_filter *)data;
	pthread_mutex_lock(&context->mutex);
	if(context->buffer == nullptr){
		context->buffer = buffer;
	}
	context->buffer_configured = false;
	if (context->fd >= 0) {
		try_configure_buffer(context, buffer);
	}
	pthread_mutex_unlock(&context->mutex);
}

static void on_remove_buffer(void *data, struct pw_buffer *buffer)
{
	struct pw_filter *context = (pw_filter *)data;
	pthread_mutex_lock(&context->mutex);
	if (context->buffer == buffer) {
		context->buffer = nullptr;
		context->buffer_configured = false;
	}
	pthread_mutex_unlock(&context->mutex);
}

static void on_process(void *data)
{
	struct pw_filter *context = (pw_filter *)data;

	struct pw_buffer *pw_buffer = pw_stream_dequeue_buffer(context->stream);
	if (!pw_buffer)
		return;

	int res = pw_stream_queue_buffer(context->stream, pw_buffer);
	if (res < 0) {
		blog(LOG_ERROR, "[pipewire] queue_buffer failed: %d", res);
	}
}

static void on_stream_param_changed(void *data, uint32_t id, const struct spa_pod *param){
	struct pw_filter *context = (pw_filter *)data;

	if (!param || id != SPA_PARAM_Format)
		return;

	struct spa_video_info_raw info;
	if(spa_format_video_raw_parse(param, &info) < 0 || (context->buffer_configured && context->format.format == info.format && context->format.size.width == info.size.width && context->format.size.height == info.size.height && context->format.framerate.num == info.framerate.num && context->format.framerate.denom == info.framerate.denom && context->format.modifier == info.modifier))
		return;

	context->format = info;

	const struct spa_pod_prop *mod_prop = spa_pod_find_prop(param, NULL, SPA_FORMAT_VIDEO_modifier);

	uint8_t buffer[1024];
	struct spa_pod_builder pw_buffer = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	const struct spa_pod *params[1];
	if(mod_prop && (mod_prop->flags & SPA_POD_PROP_FLAG_DONT_FIXATE)){
		struct spa_rectangle size = SPA_RECTANGLE(context->width, context->height);
		struct spa_fraction fps   = SPA_FRACTION((uint32_t)context->fps, 1);

		params[0] = (const spa_pod *)spa_pod_builder_add_object(&pw_buffer,
			SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
			SPA_FORMAT_mediaType,       SPA_POD_Id(SPA_MEDIA_TYPE_video),
			SPA_FORMAT_mediaSubtype,    SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
			SPA_FORMAT_VIDEO_format,    SPA_POD_Id(drm_to_spa(context->forcc)),
			SPA_FORMAT_VIDEO_modifier,  SPA_POD_Long(context->modifier),
			SPA_FORMAT_VIDEO_size,      SPA_POD_Rectangle(&size),
			SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&fps),
			0);

		pw_stream_update_params(context->stream, params, 1);
		return;
	}
	params[0] = (const spa_pod*)spa_pod_builder_add_object(&pw_buffer,
															SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
															SPA_PARAM_BUFFERS_buffers,   SPA_POD_Int(1),
															SPA_PARAM_BUFFERS_blocks,    SPA_POD_Int(1),
															SPA_PARAM_BUFFERS_size,      SPA_POD_Int(info.size.height * context->stride),
															SPA_PARAM_BUFFERS_stride,    SPA_POD_Int(context->stride),
															SPA_PARAM_BUFFERS_dataType,  SPA_POD_CHOICE_FLAGS_Int(1 << SPA_DATA_DmaBuf),
															0);
	pw_stream_update_params(context->stream, params, 1);
}

static void create_pw_stream(void *data){
	struct pw_filter *context = (pw_filter *)data;
	std::string name = "spout:" + context->ssender_name;
	blog(LOG_INFO, "[pipewire filter] creating pw node...");
	pw_thread_loop_lock(context->loop);
	if(context->stream){
		pw_stream_destroy(context->stream);
		context->stream = NULL;
	}
	struct pw_properties *props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Video",
													PW_KEY_MEDIA_CLASS, "Stream/Output/Video",
													PW_KEY_MEDIA_ROLE, "Production",
													PW_KEY_PRIORITY_DRIVER, "1",
													PW_KEY_NODE_NAME, name.c_str(),
													PW_KEY_NODE_DESCRIPTION, "OBS Layer",
													PW_KEY_NODE_NICK, name.c_str(),
													PW_KEY_MEDIA_NAME, name.c_str(),
													NULL);
	static const struct pw_stream_events events = {
		.version = PW_VERSION_STREAM_EVENTS,
		.state_changed = on_stream_state_changed,
		.param_changed = on_stream_param_changed,
		.add_buffer = on_add_buffer,
		.remove_buffer = on_remove_buffer,
		.process = on_process,
	};
	context->stream = pw_stream_new_simple(pw_thread_loop_get_loop(context->loop),name.c_str(),props,&events,context);

	uint8_t buffer[1024];
	struct spa_pod_builder pw_buffer = SPA_POD_BUILDER_INIT(buffer,sizeof(buffer));
	const struct spa_pod *params[1];

	struct spa_rectangle size = SPA_RECTANGLE(context->width, context->height);
	struct spa_fraction fps = SPA_FRACTION((uint32_t)context->fps, 1);

	params[0]= (const spa_pod*)spa_pod_builder_add_object(&pw_buffer,
														SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
														SPA_FORMAT_mediaType,    SPA_POD_Id(SPA_MEDIA_TYPE_video),
														SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
														SPA_FORMAT_VIDEO_format, SPA_POD_Id(drm_to_spa(context->forcc)),
														SPA_FORMAT_VIDEO_modifier,  SPA_POD_Long(context->modifier),
														SPA_FORMAT_VIDEO_size,   SPA_POD_Rectangle(&size),
														SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&fps),
														0);
	int status = pw_stream_connect(context->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY, (enum pw_stream_flags)(PW_STREAM_FLAG_ALLOC_BUFFERS | PW_STREAM_FLAG_RT_PROCESS), params, 1);
	pw_thread_loop_unlock(context->loop);
	if(status < 0){
		blog(LOG_ERROR, "[pipewire filter] pipewire stream connect error: %d", status);
	}
}

void init_on_render_thread(void *data){
	struct pw_filter *context = (pw_filter *)data;
	obs_source_t *target = obs_filter_get_target(context->source_context);
	if(context->is_changed){
		context->is_changed = false;
		eglDestroyImage(context->display, context->image);
		context->image = EGL_NO_IMAGE;
		close(context->fd);
		context->fd = -1;
		context->buffer_configured = false;
		gs_texrender_reset(context->texrender);
		goto change_init;
	}
	context->egl_context = eglGetCurrentContext();
	context->display = eglGetCurrentDisplay();
	blog(LOG_INFO, "[pipewire filter] texture created, attempting to extract target for height and width...");
	context->target_render = target?target:obs_filter_get_parent(context->source_context);
	blog(LOG_INFO, "[pipewire filter] target extracted, attempting to extract height and width...");
	context->width = obs_source_get_base_width(context->target_render);
	blog(LOG_INFO, "[pipewire filter] width extracted, attempting to extract height...");
	context->height = obs_source_get_base_height(context->target_render);
	context->fps = video_output_get_frame_rate(obs_get_video());
	blog(LOG_INFO, "[pipewire filter] recieved fps is... %lf", context->fps);
	blog(LOG_INFO, "[pipewire filter] height and width extracted, redering begins...");
	context->texrender = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
	change_init:

	if (gs_texrender_begin(context->texrender, context->width, context->height)) {
		blog(LOG_INFO, "[pipewire filter] first render begins...");
		struct vec4 background;
		vec4_zero(&background);

		gs_clear(GS_CLEAR_COLOR, &background, 0.0f, 0);
		gs_ortho(0.0f, (float)context->width, 0.0f, (float)context->height, -100.0f, 100.0f);

		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

		obs_source_video_render(context->target_render);

		gs_blend_state_pop();
		gs_texrender_end(context->texrender);
	}
	blog(LOG_INFO, "[pipewire filter] egl initialized, first frame rendered, trying to get the permanent dma buf fd...");
	if(!context->is_initialised){
		//a race condition with obs graphics, sometimes during obs startup, obs graphics is not fully initialized yet, so calling obs graphics functions would cause obs to crash, just delaying the filter initializations a bit so we are almost sure that obs graphics is properly initialized
        while(!gs_get_context()){
        	blog(LOG_INFO, "[pipewire filter] waiting until the obs graphic context is ready...");
        }
    }
	gs_texture_t *tex = gs_texrender_get_texture(context->texrender);
	void *obj = gs_texture_get_obj(tex);
	context->tex_handle = *reinterpret_cast<GLuint *>(obj);
	const EGLAttrib attribs[] = {
		EGL_IMAGE_PRESERVED, EGL_TRUE,
		EGL_NONE
	};
	context->image = eglCreateImage(context->display, context->egl_context, EGL_GL_TEXTURE_2D, (EGLClientBuffer)(uintptr_t)context->tex_handle, attribs);
	if(context->image){
		blog(LOG_INFO, "[pipewire filter] finally exporting the fd...");
		eglExportDMABUFImageMESA(context->display, context->image, &context->fd, &context->stride, &context->offset);
		blog(LOG_INFO, "[pipewire filter] dma buf fd got extracted %d", context->fd);
		int num_planes = 0;
		eglExportDMABUFImageQueryMESA(context->display, context->image, &(context->forcc), &num_planes, &context->modifier);
		blog(LOG_INFO, "[pipewire filter] forcc format is: %d\n and the translated spa format is: %d", context->forcc, drm_to_spa(context->forcc));
		pthread_mutex_lock(&context->mutex);
		if(!context->is_initialised){
			create_pw_stream(context);
		}else{
			pw_thread_loop_lock(context->loop);
			uint8_t buffer[1024];
			struct spa_pod_builder pw_buffer = SPA_POD_BUILDER_INIT(buffer,sizeof(buffer));
			const struct spa_pod *params[1];

			struct spa_rectangle size = SPA_RECTANGLE(context->width, context->height);
			struct spa_fraction fps = SPA_FRACTION((uint32_t)context->fps, 1);

			params[0]= (const spa_pod*)spa_pod_builder_add_object(&pw_buffer,
																SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
																SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
																SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
																SPA_FORMAT_VIDEO_format, SPA_POD_Id(drm_to_spa(context->forcc)),
																SPA_FORMAT_VIDEO_modifier, SPA_POD_Long(context->modifier),
																SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&size),
																SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&fps),
																0);
			pw_stream_update_params(context->stream, params, 1);
			pw_thread_loop_unlock(context->loop);
		}
		try_configure_buffer(context);
		pthread_mutex_unlock(&context->mutex);
	}
	context->is_initialised = true;
}
void pw_offscreen_render(void *data, uint32_t cx, uint32_t cy)
{
	UNUSED_PARAMETER(cx);
	UNUSED_PARAMETER(cy);
	struct pw_filter *context = (pw_filter *)data;

	if (!context->is_active) {
		blog(LOG_INFO, "[pipewire filter] the video render is not active...");
		return;
	}
	context->is_active = false;

	if (!context->is_initialised || context->is_changed) {
		init_on_render_thread(context);
	}

	if(!context->buffer || !context->buffer_configured || !context->is_streaming){
		return;
	}

	pthread_mutex_lock(&context->mutex);
	gs_texrender_t *texrender = context->texrender;
	struct pw_stream *stream = context->stream;
	uint32_t height = context->height;
	uint32_t width = context->width;
	obs_source_t *target = context->target_render;
	pthread_mutex_unlock(&context->mutex);

	// Use the default effect to render it back into a format Spout accepts
	gs_texrender_reset(texrender);
	if (gs_texrender_begin(texrender, width, height)) {
		struct vec4 background;
		vec4_zero(&background);

		gs_clear(GS_CLEAR_COLOR, &background, 0.0f, 0);
		gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f, 100.0f);

		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
		obs_source_video_render(target);
		gs_blend_state_pop();
		gs_texrender_end(texrender);

		//using fence as a low latency signal that the frame is drawn completely without needing to do double buffering
		GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
		glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 16 * 1000000);
		glDeleteSync(fence);

		pw_stream_trigger_process(stream);
	}
}

void pw_filter_update(void *data, obs_data_t *settings)
{
	struct pw_filter *context = (pw_filter *)data;
	obs_remove_main_render_callback(pw_offscreen_render, context);

	const char *sender_name = obs_data_get_string(settings, FILTER_PROP_NAME);
	
	if(context->is_initialised && strcmp(context->ssender_name.c_str(), sender_name)){
		pw_thread_loop_lock(context->loop);
		blog(LOG_INFO, "[pipewire filter] changing name is triggered");
		pw_stream_set_active(context->stream, false);
		pw_stream_destroy(context->stream);
		context->stream = NULL;
		pthread_mutex_lock(&context->mutex);
		context->sender_name = sender_name;
		context->ssender_name = std::string(context->sender_name);
		context->is_changed = true;
		pthread_mutex_unlock(&context->mutex);
		create_pw_stream(context);
		pw_thread_loop_unlock(context->loop);
	}
	
	obs_add_main_render_callback(pw_offscreen_render, context);
}

void *pw_filter_create(obs_data_t *settings, obs_source_t *source)
{
	blog(LOG_INFO, "[pipewire filter] starting to create the filter");
	struct pw_filter *context = (pw_filter *)bzalloc(sizeof(pw_filter));
	new (&context->ssender_name) std::string();
	// Despite bzalloc I still want to at least initialise pointer fields
	context->source_context = nullptr;
	context->sender_name = nullptr;
	context->texrender = nullptr;
	context->stagesurface = nullptr;
	context->stream = nullptr;
	context->loop = nullptr;
	context->context = nullptr;
	context->core = nullptr;
	context->buffer = nullptr;
	context->buffer_configured = false;
	context->fd = -1;
	context->is_initialised = false;
	context->is_active = false;

	pthread_mutex_init_value(&context->mutex);
	if (pthread_mutex_init(&context->mutex, NULL) != 0) {
		blog(LOG_ERROR, "[pipewire filter] Failed to create mutex for pipewire filter!");
		pw_filter_destroy(context);
		return nullptr;
	}

	context->source_context = source;

	context->sender_name = obs_data_get_string(settings, FILTER_PROP_NAME);

	blog(LOG_INFO, "[pipewire filter] graphics initialized, initializing pipewire...");

	pw_init(NULL,NULL);
	context->ssender_name = std::string(context->sender_name);
	std::string pw_name = "spout:" + context->ssender_name;
	context->loop = pw_thread_loop_new(pw_name.c_str(), NULL);
	context->context = pw_context_new(pw_thread_loop_get_loop(context->loop), NULL, 0);
	context->core = pw_context_connect(context->context, NULL, 0);
	pw_thread_loop_start(context->loop);
	
	pw_filter_update(context, settings);

	// from this point, need to lock mutex to access context safely
	blog(LOG_INFO, "[pipewire filter] filter got created");
	return context;
}

void pw_filter_destroy(void *data)
{
	struct pw_filter *context = (pw_filter *)data;

	if (!context) {
		return;
	}

	obs_remove_main_render_callback(pw_offscreen_render, context);
	context->is_active = false;

	if(context->loop){
		pw_thread_loop_stop(context->loop);

		if(context->stream){
			pw_stream_destroy(context->stream);
			context->stream = nullptr;
		}
		if(context->core){
			pw_core_disconnect(context->core);
			context->core = nullptr;
		}
		if(context->context){
			pw_context_destroy(context->context);
			context->context = nullptr;
		}

		pw_thread_loop_destroy(context->loop);
		context->loop = nullptr;
	}

	if(context->context){
		pw_context_destroy(context->context);
		context->context = nullptr;
	}

	obs_enter_graphics();
	{
		if(context->image != EGL_NO_IMAGE_KHR){
			eglDestroyImage(context->display, context->image);
			context->image = EGL_NO_IMAGE;
		}
		if (context->texrender) {
			gs_texrender_destroy(context->texrender);
			context->texrender = nullptr;
		}
	}
	obs_leave_graphics();

	if(context->fd >= 0){
		close(context->fd);
		context->fd = -1;
	}

	if (context->stagesurface) {
		gs_stagesurface_unmap(context->stagesurface);
		gs_stagesurface_destroy(context->stagesurface);
		context->stagesurface = nullptr;
	}

	if (context->texrender) {
		gs_texrender_destroy(context->texrender);
		context->texrender = nullptr;
	}

	pthread_mutex_destroy(&context->mutex);
	context->ssender_name.~basic_string();
	bfree(context);
}

void pw_filter_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	struct pw_filter *context = (pw_filter *)data;
	if(context->is_initialised){
		obs_source_t *target = obs_filter_get_target(context->source_context);
		obs_source_t *ctarget_render = target?target:obs_filter_get_parent(context->source_context);
		uint32_t cwidth = obs_source_get_base_width(ctarget_render);
		uint32_t cheight =  obs_source_get_base_height(ctarget_render);
		double cfps = video_output_get_frame_rate(obs_get_video());
		if(!cwidth && !cheight){
			//ERROR HANDLE: OBS for resetting some sources for whatever reason, returns 0 at the beginning for both width and height, causing crash in pipewire.
			return;
		}
		if(context->width != cwidth){
			context->width = cwidth;
			context->is_changed = true;
		}
		if(context->height != cheight){
			context->height = cheight;
			context->is_changed = true;
		}
		if(context->fps != cfps){
			context->fps = cfps;
			context->is_changed = true;
		}
		if(context->target_render != ctarget_render){
			context->target_render = ctarget_render;
		}
	}
}

bool pw_filter_change_name(obs_properties_t *, obs_property_t *, void *data)
{
	struct pw_filter *context = (pw_filter *)data;
	obs_data_t *settings = obs_source_get_settings(context->source_context);
	pw_filter_update(context, settings);
	obs_data_release(settings);
	return true;
}

obs_properties_t *pw_filter_getproperties(void *data)
{
	//UNUSED_PARAMETER(unused);
	obs_properties_t *props = obs_properties_create();
	obs_properties_set_flags(props, OBS_PROPERTIES_DEFER_UPDATE);
	obs_properties_add_text(props, FILTER_PROP_NAME, obs_module_text("spoutname"), OBS_TEXT_DEFAULT);
	obs_properties_add_button2(props, "pw_apply", obs_module_text("changename"), pw_filter_change_name, data);
	return props;
}
void pw_filter_videorender(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct pw_filter *context = (pw_filter *)data;

	pthread_mutex_lock(&context->mutex);

	context->is_active = true;

	pthread_mutex_unlock(&context->mutex);

	obs_source_skip_video_filter(context->source_context);
}

extern "C" struct obs_source_info create_pw_filter_info()
{
	struct obs_source_info pw_filter_info = {};
	pw_filter_info.id = "pw_filter";
	pw_filter_info.type = OBS_SOURCE_TYPE_FILTER;
	pw_filter_info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
	pw_filter_info.get_name = pw_filter_getname;
	pw_filter_info.get_properties = pw_filter_getproperties;
	pw_filter_info.get_defaults = pw_filter_getdefaults;
	pw_filter_info.create = pw_filter_create;
	pw_filter_info.destroy = pw_filter_destroy;
	pw_filter_info.update = pw_filter_update;
	pw_filter_info.video_tick = pw_filter_tick;
	pw_filter_info.video_render = pw_filter_videorender;

	return pw_filter_info;
}
