#ifndef RENDER_OGL_H
#define RENDER_OGL_H

#include <windows.h>
#include "opengl_utils.h"

#define TEXTURE_COUNT 4
#define FBO_COUNT 2
#define MAX_SHADER_PASSES 16
#define MAX_SHADER_FBO (MAX_SHADER_PASSES + 1)

#ifdef _DEBUG
#define GL_CHECK(stmt) do { \
            stmt; \
            ogl_check_error(#stmt); \
        } while (0)
#else
#define GL_CHECK(stmt) stmt
#endif

typedef struct OGLRENDERER
{
    HWND hwnd;
    HDC hdc;
    HGLRC context;
    GLuint main_program;
    GLuint shader_programs[MAX_SHADER_PASSES];
    int shader_pass_count;
    BOOL got_error;
    int surface_tex_width;
    int surface_tex_height;
    GLenum surface_format;
    GLenum surface_type;
    GLuint surface_tex_ids[TEXTURE_COUNT];
    GLuint palette_tex_ids[TEXTURE_COUNT];
    float scale_w;
    float scale_h;
    GLint main_tex_coord_attr_loc;
    GLint main_vertex_coord_attr_loc;
    GLuint main_vbos[3];
    GLuint main_vao;
    GLint shader_frame_count_uni_loc[MAX_SHADER_PASSES];
    GLuint frame_buffer_id[MAX_SHADER_FBO];
    GLuint frame_buffer_tex_id[MAX_SHADER_FBO];
    GLint shader_tex_coord_attr_loc[MAX_SHADER_PASSES];
    GLuint shader_vbos[MAX_SHADER_PASSES][3];
    GLuint shader_vao[MAX_SHADER_PASSES];
    BOOL use_opengl;
    BOOL filter_bilinear;
    BOOL shader_upscale[MAX_SHADER_PASSES];
    BOOL shader_linear_filter[MAX_SHADER_PASSES];
    BOOL preset_active;
} OGLRENDERER;

DWORD WINAPI ogl_render_main(void);
BOOL ogl_create();
BOOL ogl_release();

#endif
