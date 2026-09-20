#include <waywallen_display_egl_presenter.h>

#include <assert.h>
#include <stdio.h>

typedef struct fake_gl {
    uint32_t next_name;
    unsigned textures_created;
    unsigned textures_deleted;
    unsigned framebuffers_created;
    unsigned framebuffers_deleted;
    unsigned blits;
} fake_gl_t;

static void get_integer(void* user_data, uint32_t name, int32_t* value) {
    (void)user_data;
    (void)name;
    *value = 0;
}

static bool is_enabled(void* user_data, uint32_t capability) {
    (void)user_data;
    (void)capability;
    return false;
}

static void capability(void* user_data, uint32_t value) {
    (void)user_data;
    (void)value;
}

static void gen_textures(void* user_data, int32_t count, uint32_t* textures) {
    fake_gl_t* gl = user_data;
    for (int32_t index = 0; index < count; ++index) textures[index] = ++gl->next_name;
    gl->textures_created += (unsigned)count;
}

static void delete_textures(void* user_data, int32_t count, const uint32_t* textures) {
    fake_gl_t* gl = user_data;
    (void)textures;
    gl->textures_deleted += (unsigned)count;
}

static void bind_object(void* user_data, uint32_t target, uint32_t object) {
    (void)user_data;
    (void)target;
    (void)object;
}

static void texture_parameter(void* user_data, uint32_t target, uint32_t name, int32_t value) {
    (void)user_data;
    (void)target;
    (void)name;
    (void)value;
}

static void texture_image_2d(void* user_data, uint32_t target, int32_t level,
                             int32_t internal_format, int32_t width, int32_t height, int32_t border,
                             uint32_t format, uint32_t type, const void* data) {
    (void)user_data;
    (void)target;
    (void)level;
    (void)internal_format;
    (void)width;
    (void)height;
    (void)border;
    (void)format;
    (void)type;
    (void)data;
}

static void gen_framebuffers(void* user_data, int32_t count, uint32_t* framebuffers) {
    fake_gl_t* gl = user_data;
    for (int32_t index = 0; index < count; ++index) framebuffers[index] = ++gl->next_name;
    gl->framebuffers_created += (unsigned)count;
}

static void delete_framebuffers(void* user_data, int32_t count, const uint32_t* framebuffers) {
    fake_gl_t* gl = user_data;
    (void)framebuffers;
    gl->framebuffers_deleted += (unsigned)count;
}

static void framebuffer_texture_2d(void* user_data, uint32_t target, uint32_t attachment,
                                   uint32_t texture_target, uint32_t texture, int32_t level) {
    (void)user_data;
    (void)target;
    (void)attachment;
    (void)texture_target;
    (void)texture;
    (void)level;
}

static uint32_t check_framebuffer(void* user_data, uint32_t target) {
    (void)user_data;
    (void)target;
    return 0x8CD5;
}

static void blit_framebuffer(void* user_data, int32_t source_x0, int32_t source_y0,
                             int32_t source_x1, int32_t source_y1, int32_t dest_x0, int32_t dest_y0,
                             int32_t dest_x1, int32_t dest_y1, uint32_t mask, uint32_t filter) {
    fake_gl_t* gl = user_data;
    (void)source_x0;
    (void)source_y0;
    (void)source_x1;
    (void)source_y1;
    (void)dest_x0;
    (void)dest_y0;
    (void)dest_x1;
    (void)dest_y1;
    (void)mask;
    (void)filter;
    ++gl->blits;
}

static uint32_t get_error(void* user_data) {
    (void)user_data;
    return 0;
}

static const waywallen_gl_functions_t functions = {
    .get_integer            = get_integer,
    .is_enabled             = is_enabled,
    .enable                 = capability,
    .disable                = capability,
    .gen_textures           = gen_textures,
    .delete_textures        = delete_textures,
    .bind_texture           = bind_object,
    .texture_parameter      = texture_parameter,
    .texture_image_2d       = texture_image_2d,
    .gen_framebuffers       = gen_framebuffers,
    .delete_framebuffers    = delete_framebuffers,
    .bind_framebuffer       = bind_object,
    .framebuffer_texture_2d = framebuffer_texture_2d,
    .check_framebuffer      = check_framebuffer,
    .blit_framebuffer       = blit_framebuffer,
    .get_error              = get_error,
};

int main(void) {
    fake_gl_t                  gl        = { 0 };
    waywallen_egl_presenter_t* presenter = NULL;
    assert(waywallen_egl_presenter_create(&functions, &gl, &presenter) == 0);

    assert(waywallen_egl_presenter_prepare(presenter, 100, 640, 480, false, false) ==
           WAYWALLEN_EGL_PREPARE_CANDIDATE_READY);
    waywallen_egl_presentable_t* outgoing = NULL;
    assert(waywallen_egl_presenter_commit(presenter, &outgoing) == 0);
    assert(outgoing == NULL);
    assert(waywallen_egl_presenter_prepare(presenter, 101, 640, 480, false, false) ==
           WAYWALLEN_EGL_PREPARE_CURRENT_UPDATED);

    assert(waywallen_egl_presenter_prepare(presenter, 102, 800, 600, true, false) ==
           WAYWALLEN_EGL_PREPARE_CANDIDATE_READY);
    const unsigned textures_created = gl.textures_created;
    assert(waywallen_egl_presenter_prepare(presenter, 103, 800, 600, true, true) ==
           WAYWALLEN_EGL_PREPARE_CANDIDATE_READY);
    assert(gl.textures_created == textures_created);
    assert(waywallen_egl_presenter_commit(presenter, &outgoing) == 0);
    assert(outgoing != NULL);

    waywallen_egl_presentable_descriptor_t descriptor = { 0 };
    assert(waywallen_egl_presentable_descriptor(outgoing, &descriptor));
    assert(descriptor.width == 640 && descriptor.height == 480);
    waywallen_egl_presenter_release(presenter, outgoing);
    assert(gl.textures_deleted == 1);
    assert(gl.framebuffers_deleted == 1);

    waywallen_egl_presenter_destroy(presenter);
    assert(gl.textures_created == 2);
    assert(gl.textures_deleted == 2);
    assert(gl.framebuffers_created == 3);
    assert(gl.framebuffers_deleted == 3);
    assert(gl.blits == 4);
    puts("test_egl_presenter: OK");
    return 0;
}
