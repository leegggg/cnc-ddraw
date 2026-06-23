#include <windows.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include "config.h"
#include "fps_limiter.h"
#include "opengl_utils.h"
#include "dd.h"
#include "ddsurface.h"
#include "openglshader.h"
#include "render_gdi.h"
#include "render_ogl.h"
#include "utils.h"
#include "debug.h"


static HGLRC ogl_create_core_context(HDC hdc);
static void ogl_build_programs();
static void ogl_create_textures(int width, int height);
static void ogl_init_main_program();
static void ogl_init_shader_programs();
static void ogl_render();
static BOOL ogl_release_resources();
static BOOL ogl_texture_upload_test();
static BOOL ogl_shader_test();
static void ogl_check_error(const char* stmt);

typedef struct OGLPRESET
{
    BOOL is_preset;
    int pass_count;
    BOOL has_shader_path[MAX_SHADER_PASSES];
    char shader_path[MAX_SHADER_PASSES][MAX_PATH];
    BOOL has_linear_filter[MAX_SHADER_PASSES];
    BOOL linear_filter[MAX_SHADER_PASSES];
    BOOL has_scale_type[MAX_SHADER_PASSES];
    BOOL scale_is_source[MAX_SHADER_PASSES];
} OGLPRESET;

static OGLPRESET ogl_parse_shader_preset(const char* shader_path);
static BOOL ogl_try_resolve_shader_path(const char* base_dir, const char* value, char* out, size_t out_size);
static char* ogl_trim(char* str);
static BOOL ogl_ends_with(const char* str, const char* suffix);

static OGLRENDERER g_ogl;

static char* ogl_trim(char* str)
{
    if (!str)
        return str;

    while (*str && isspace((unsigned char)*str))
        str++;

    char* end = str + strlen(str);
    while (end > str && isspace((unsigned char)*(end - 1)))
        end--;

    *end = '\0';
    return str;
}

static BOOL ogl_ends_with(const char* str, const char* suffix)
{
    if (!str || !suffix)
        return FALSE;

    size_t str_len = strlen(str);
    size_t suffix_len = strlen(suffix);

    if (suffix_len > str_len)
        return FALSE;

    return _stricmp(str + str_len - suffix_len, suffix) == 0;
}

static BOOL ogl_try_resolve_shader_path(const char* base_dir, const char* value, char* out, size_t out_size)
{
    if (!value || !out || out_size == 0)
        return FALSE;

    out[0] = '\0';

    if (GetFileAttributes(value) != INVALID_FILE_ATTRIBUTES)
    {
        strncpy(out, value, out_size - 1);
        out[out_size - 1] = '\0';
        return TRUE;
    }

    if (base_dir && base_dir[0])
    {
        _snprintf(out, out_size - 1, "%s%s", base_dir, value);
        out[out_size - 1] = '\0';

        if (GetFileAttributes(out) != INVALID_FILE_ATTRIBUTES)
            return TRUE;
    }

    if (g_config.dll_path[0])
    {
        _snprintf(out, out_size - 1, "%s%s", g_config.dll_path, value);
        out[out_size - 1] = '\0';

        if (GetFileAttributes(out) != INVALID_FILE_ATTRIBUTES)
            return TRUE;
    }

    out[0] = '\0';
    return FALSE;
}

static OGLPRESET ogl_parse_shader_preset(const char* shader_path)
{
    OGLPRESET preset = { 0 };

    if (!shader_path || !ogl_ends_with(shader_path, ".glslp"))
        return preset;

    FILE* file = fopen(shader_path, "rb");
    if (!file)
        return preset;

    preset.is_preset = TRUE;

    char base_dir[MAX_PATH] = { 0 };
    strncpy(base_dir, shader_path, sizeof(base_dir) - 1);
    base_dir[sizeof(base_dir) - 1] = '\0';

    char* slash = strrchr(base_dir, '\\');
    if (slash)
    {
        slash[1] = '\0';
    }
    else
    {
        base_dir[0] = '\0';
    }

    char line[1024];
    while (fgets(line, sizeof(line), file))
    {
        char* p = line;

        if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF)
            p += 3;

        p = ogl_trim(p);
        if (!p[0] || p[0] == '#' || p[0] == ';')
            continue;

        char* eq = strchr(p, '=');
        if (!eq)
            continue;

        *eq = '\0';

        char* key = ogl_trim(p);
        char* value = ogl_trim(eq + 1);

        if (value[0] == '"')
        {
            value++;
            char* end_quote = strrchr(value, '"');
            if (end_quote)
                *end_quote = '\0';
        }

        if (_stricmp(key, "shaders") == 0)
        {
            preset.pass_count = atoi(value);
            continue;
        }

        if (_strnicmp(key, "shader", 6) == 0 && isdigit((unsigned char)key[6]))
        {
            int index = atoi(key + 6);
            if (index >= 0 && index < MAX_SHADER_PASSES)
            {
                if (ogl_try_resolve_shader_path(base_dir, value, preset.shader_path[index], sizeof(preset.shader_path[index])))
                {
                    preset.has_shader_path[index] = TRUE;
                }
            }

            continue;
        }

        if (_strnicmp(key, "filter_linear", 13) == 0 && isdigit((unsigned char)key[13]))
        {
            int index = atoi(key + 13);
            if (index >= 0 && index < MAX_SHADER_PASSES)
            {
                preset.has_linear_filter[index] = TRUE;
                preset.linear_filter[index] =
                    _stricmp(value, "true") == 0 || _stricmp(value, "1") == 0 || _stricmp(value, "yes") == 0;
            }

            continue;
        }

        if (_strnicmp(key, "scale_type", 10) == 0 && isdigit((unsigned char)key[10]))
        {
            int index = atoi(key + 10);
            if (index >= 0 && index < MAX_SHADER_PASSES)
            {
                preset.has_scale_type[index] = TRUE;
                preset.scale_is_source[index] = _stricmp(value, "source") == 0;
            }

            continue;
        }
    }

    fclose(file);
    return preset;
}

BOOL ogl_create()
{
    if (g_ogl.hwnd == g_ddraw.hwnd && g_ogl.hdc == g_ddraw.render.hdc && g_ogl.context)
    {
        return TRUE;
    }

    ogl_release();

    g_ogl.context = xwglCreateContext(g_ddraw.render.hdc);
    if (g_ogl.context)
    {
        g_ogl.hwnd = g_ddraw.hwnd;
        g_ogl.hdc = g_ddraw.render.hdc;

        GLenum err = GL_NO_ERROR;
        BOOL made_current = FALSE;

        for (int i = 0; i < 5; i++)
        {
            if ((made_current = xwglMakeCurrent(g_ogl.hdc, g_ogl.context)))
                break;

            Sleep(50);
        }

        if (made_current && (err = glGetError()) == GL_NO_ERROR)
        {
            GL_CHECK(oglu_init());

            TRACE("+--OpenGL-----------------------------------------\n");
            TRACE("| GL_VERSION:                  %s\n", glGetString(GL_VERSION));
            TRACE("| GL_VENDOR:                   %s\n", glGetString(GL_VENDOR));
            TRACE("| GL_RENDERER:                 %s\n", glGetString(GL_RENDERER));
            TRACE("| GL_SHADING_LANGUAGE_VERSION: %s\n", glGetString(GL_SHADING_LANGUAGE_VERSION));
            TRACE("+------------------------------------------------\n");

#ifdef _DEBUG
            while (glGetError() != GL_NO_ERROR); /* Ignore errors from glGetString */
#endif

            GL_CHECK(g_ogl.context = ogl_create_core_context(g_ogl.hdc));
        }
        else
        {
            TRACE("OpenGL error %08x, GetLastError %lu (xwglMakeCurrent())\n", err, GetLastError());
            ogl_check_error("xwglMakeCurrent()");
        }

        for (int i = 0; i < 5; i++)
        {
            if (xwglMakeCurrent(NULL, NULL))
                break;

            Sleep(50);
        }

        return TRUE;
    }

    g_ogl.hwnd = NULL;
    g_ogl.hdc = NULL;

    return FALSE;
}

DWORD WINAPI ogl_render_main(void)
{
    Sleep(250);
    g_ogl.got_error = g_ogl.use_opengl = FALSE;
    GLenum err = GL_NO_ERROR;
    BOOL made_current = FALSE;

    for (int i = 0; i < 5; i++)
    {
        if ((made_current = xwglMakeCurrent(g_ogl.hdc, g_ogl.context)))
            break;

        Sleep(50);
    }

    if (made_current && (err = glGetError()) == GL_NO_ERROR)
    {
        GL_CHECK(oglu_init());

        g_ogl.got_error = g_ogl.got_error || (err = glGetError()) != GL_NO_ERROR;

        BOOL got_swap_ctrl;
        GL_CHECK(got_swap_ctrl = oglu_ext_exists("WGL_EXT_swap_control", g_ogl.hdc));

        if (got_swap_ctrl && wglSwapIntervalEXT)
            wglSwapIntervalEXT(g_config.vsync ? 1 : 0);

        fpsl_init();
        GL_CHECK(ogl_build_programs());
        GL_CHECK(ogl_create_textures(g_ddraw.width, g_ddraw.height));
        GL_CHECK(ogl_init_main_program());
        GL_CHECK(ogl_init_shader_programs());

        g_ogl.got_error = g_ogl.got_error || (err = glGetError()) != GL_NO_ERROR;
        GL_CHECK(g_ogl.got_error = g_ogl.got_error || !ogl_texture_upload_test());
        GL_CHECK(g_ogl.got_error = g_ogl.got_error || !ogl_shader_test());
        g_ogl.got_error = g_ogl.got_error || (err = glGetError()) != GL_NO_ERROR;

        g_ogl.use_opengl = (g_ogl.main_program || g_ddraw.bpp == 16 || g_ddraw.bpp == 32) && !g_ogl.got_error;

        GL_CHECK(ogl_render());

        GL_CHECK(ogl_release_resources());

        while (glGetError() != GL_NO_ERROR);
    }
    else
    {
        TRACE("OpenGL error %08x, GetLastError %lu (xwglMakeCurrent())\n", err, GetLastError());
        ogl_check_error("xwglMakeCurrent()");
    }

    for (int i = 0; i < 5; i++)
    {
        if (xwglMakeCurrent(NULL, NULL))
            break;

        Sleep(50);
    }
    
    if (!g_ogl.use_opengl)
    {
        g_ddraw.show_driver_warning = TRUE;
        g_ddraw.renderer = gdi_render_main;
        gdi_render_main();
    }

    return 0;
}


static void ogl_check_error(const char* stmt)
{
#ifdef _DEBUG
    GLenum err;
    while ((err = glGetError()) != GL_NO_ERROR)
    {
        g_ogl.got_error = TRUE;
        TRACE("OpenGL error %08x (%s)\n", err, stmt);
    }
#endif
}

static HGLRC ogl_create_core_context(HDC hdc)
{
    if (!wglCreateContextAttribsARB)
        return g_ogl.context;

    int attribs[] = {
        WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
        WGL_CONTEXT_MINOR_VERSION_ARB, 2,
        WGL_CONTEXT_FLAGS_ARB, WGL_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB,
        WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
        0 };

    HGLRC context = wglCreateContextAttribsARB(hdc, 0, attribs);
    BOOL made_current = context && xwglMakeCurrent(hdc, context);

    if (made_current)
    {
        xwglDeleteContext(g_ogl.context);
        oglu_init();

        TRACE("+--OpenGL Core-----------------------------------\n");
        TRACE("| GL_VERSION:                  %s\n", glGetString(GL_VERSION));
        TRACE("| GL_VENDOR:                   %s\n", glGetString(GL_VENDOR));
        TRACE("| GL_RENDERER:                 %s\n", glGetString(GL_RENDERER));
        TRACE("| GL_SHADING_LANGUAGE_VERSION: %s\n", glGetString(GL_SHADING_LANGUAGE_VERSION));
        TRACE("+------------------------------------------------\n");

        return context;
    }
    else if (context)
    {
        xwglDeleteContext(context);
    }

    return g_ogl.context;
}

static void ogl_build_programs()
{
    g_ogl.main_program = 0;
    g_ogl.shader_pass_count = 0;
    g_ogl.preset_active = FALSE;

    for (int i = 0; i < MAX_SHADER_PASSES; i++)
    {
        g_ogl.shader_programs[i] = 0;
        g_ogl.shader_upscale[i] = FALSE;
        g_ogl.shader_linear_filter[i] = FALSE;
        g_ogl.shader_frame_count_uni_loc[i] = -1;
        g_ogl.shader_tex_coord_attr_loc[i] = -1;
    }

    BOOL core_profile = wglCreateContextAttribsARB != NULL;

    if (g_oglu_got_version3)
    {
        if (g_ddraw.bpp == 8)
        {
            g_ogl.main_program = oglu_build_program(PASSTHROUGH_VERT_SHADER, PALETTE_FRAG_SHADER, core_profile);
        }
        else if (g_ddraw.bpp == 16 && g_config.rgb555)
        {
            g_ogl.main_program = oglu_build_program(PASSTHROUGH_VERT_SHADER, RGB555_FRAG_SHADER, core_profile);
        }
        else if (g_ddraw.bpp == 16 || g_ddraw.bpp == 32)
        {
            g_ogl.main_program = oglu_build_program(PASSTHROUGH_VERT_SHADER, PASSTHROUGH_FRAG_SHADER, core_profile);
        }

        BOOL bilinear = FALSE;
        char shader_path[MAX_PATH] = { 0 };
        OGLPRESET preset = { 0 };

        if (g_ogl.main_program)
        {
            strncpy(shader_path, g_config.shader, sizeof(shader_path));
            shader_path[sizeof(shader_path) - 1] = '\0'; /* strncpy fix */

            if (GetFileAttributes(shader_path) == INVALID_FILE_ATTRIBUTES)
            {
                _snprintf(shader_path, sizeof(shader_path) - 1, "%s%s", g_config.dll_path, g_config.shader);
            }

            /* Hack for Intel HD 4000 driver bug - force default shader */

            if (_stricmp(g_oglu_version_long, "4.0.0 - Build 10.18.10.4252") == 0 ||
                _stricmp(g_oglu_version_long, "4.0.0 - Build 10.18.10.5161") == 0)
            {
                //shader_path[0] = 0;
                //g_config.shader[0] = 0;
            }

            /* detect common upscaling shaders and disable them if no upscaling is required */

            BOOL is_upscaler =
                strstr(g_config.shader, "fsr.glsl") != NULL ||
                strstr(g_config.shader, "catmull-rom-bilinear.glsl") != NULL ||
                strstr(g_config.shader, "lanczos2-sharp.glsl") != NULL ||
                strstr(g_config.shader, "xbr-lv2-noblend.glsl") != NULL ||
                strstr(g_config.shader, "xbrz-freescale-multipass.glsl") != NULL ||
                strstr(g_config.shader, "xbrz-freescale.glsl") != NULL;

            if (!is_upscaler ||
                g_ddraw.render.viewport.width != g_ddraw.width ||
                g_ddraw.render.viewport.height != g_ddraw.height ||
                g_config.vhack)
            {
                preset = ogl_parse_shader_preset(shader_path);
                g_ogl.preset_active = preset.is_preset;

                if (preset.is_preset)
                {
                    int pass_limit = preset.pass_count > 0 ? preset.pass_count : MAX_SHADER_PASSES;
                    if (pass_limit > MAX_SHADER_PASSES)
                        pass_limit = MAX_SHADER_PASSES;

                    for (int i = 0; i < pass_limit; i++)
                    {
                        if (!preset.has_shader_path[i])
                            continue;

                        g_ogl.shader_programs[i] = oglu_build_program_from_file(preset.shader_path[i], core_profile);
                        if (!g_ogl.shader_programs[i])
                            break;

                        g_ogl.shader_pass_count = i + 1;

                        if (preset.has_linear_filter[i])
                            g_ogl.shader_linear_filter[i] = preset.linear_filter[i];

                        if ((strstr(preset.shader_path[i], "xbrz-freescale-multipass") != NULL ||
                            strstr(preset.shader_path[i], "-pass1scale") != NULL) ||
                            (preset.has_scale_type[i] && preset.scale_is_source[i]))
                        {
                            g_ogl.shader_upscale[i] = TRUE;
                        }
                    }
                }
                else
                {
                    g_ogl.shader_programs[0] = oglu_build_program_from_file(shader_path, core_profile);

                    if (g_ogl.shader_programs[0])
                        g_ogl.shader_pass_count = 1;
                }

                if (g_ogl.shader_programs[0] && 
                    (strstr(g_config.shader, "xbrz-freescale-multipass.glsl") != NULL || 
                        strstr(g_config.shader, "-pass1scale") != NULL))
                {
                    g_ogl.shader_upscale[0] = TRUE;
                }

                if (!g_ogl.shader_programs[0] &&
                    (g_ddraw.render.viewport.width != g_ddraw.width ||
                        g_ddraw.render.viewport.height != g_ddraw.height ||
                        g_config.vhack))
                {
                    g_ogl.shader_programs[0] = 
                        oglu_build_program(
                            _stricmp(g_config.shader, "xBR-lv2") == 0 ? XBR_LV2_VERT_SHADER :
                            PASSTHROUGH_VERT_SHADER, 
                            _stricmp(g_config.shader, "Nearest neighbor") == 0 ? PASSTHROUGH_FRAG_SHADER :
                            _stricmp(g_config.shader, "Bilinear") == 0 ? PASSTHROUGH_FRAG_SHADER :
                            _stricmp(g_config.shader, "Lanczos") == 0 ? LANCZOS2_FRAG_SHADER :
                            _stricmp(g_config.shader, "xBR-lv2") == 0 ? XBR_LV2_FRAG_SHADER :
                            CATMULL_ROM_FRAG_SHADER, 
                            core_profile);

                    if (g_ogl.shader_programs[0])
                        g_ogl.shader_pass_count = 1;

                    bilinear =
                        _stricmp(g_config.shader, "Nearest neighbor") != 0 && 
                        _stricmp(g_config.shader, "Lanczos") != 0 &&
                        _stricmp(g_config.shader, "xBR-lv2") != 0;
                }
            }
        }
        else
        {
            g_oglu_got_version3 = FALSE;
        }

        if (g_ogl.shader_programs[0] && !preset.is_preset)
        {
            if (strlen(shader_path) <= sizeof(shader_path) - 8)
            {
                strcat(shader_path, ".pass1");

                g_ogl.shader_programs[1] = oglu_build_program_from_file(shader_path, core_profile);
                if (g_ogl.shader_programs[1])
                {
                    g_ogl.shader_pass_count = 2;

                    if (strstr(shader_path, "xbrz-freescale-multipass") != NULL ||
                        strstr(shader_path, "-pass1scale") != NULL)
                    {
                        g_ogl.shader_upscale[1] = TRUE;
                    }
                }
            }
        }

        if (preset.is_preset)
        {
            if (preset.has_linear_filter[0])
                g_ogl.filter_bilinear = preset.linear_filter[0];
            else
                g_ogl.filter_bilinear = strstr(g_config.shader, "bilinear.glsl") != NULL || bilinear;

            if (preset.pass_count > MAX_SHADER_PASSES)
            {
                TRACE("OpenGL shader preset has %d passes, limiting to %d\n", preset.pass_count, MAX_SHADER_PASSES);
            }
        }
        else
        {
            g_ogl.filter_bilinear = strstr(g_config.shader, "bilinear.glsl") != NULL || bilinear;
        }
    }

    if (g_oglu_got_version2 && !g_ogl.main_program)
    {
        if (g_ddraw.bpp == 8)
        {
            g_ogl.main_program = oglu_build_program(PASSTHROUGH_VERT_SHADER_110, PALETTE_FRAG_SHADER_110, FALSE);
        }
        else if (g_ddraw.bpp == 16 || g_ddraw.bpp == 32)
        {
            g_ogl.main_program = oglu_build_program(PASSTHROUGH_VERT_SHADER_110, PASSTHROUGH_FRAG_SHADER_110, FALSE);
        }
    }
}

static void ogl_create_textures(int width, int height)
{
    GLenum err = GL_NO_ERROR;

    int w = g_ogl.shader_pass_count > 1 ? max(width, g_ddraw.render.viewport.width) : width;
    int h = g_ogl.shader_pass_count > 1 ? max(height, g_ddraw.render.viewport.height) : height;

    g_ogl.surface_tex_width =
        w <= 1024 ? 1024 : w <= 2048 ? 2048 : w <= 4096 ? 4096 : w <= 8192 ? 8192 : w;

    g_ogl.surface_tex_height =
        h <= 512 ? 512 : h <= 1024 ? 1024 : h <= 2048 ? 2048 : h <= 4096 ? 4096 : h <= 8192 ? 8192 : h;

    g_ogl.scale_w = (float)width / g_ogl.surface_tex_width;
    g_ogl.scale_h = (float)height / g_ogl.surface_tex_height;

    glGenTextures(TEXTURE_COUNT, g_ogl.surface_tex_ids);

    for (int i = 0; i < TEXTURE_COUNT; i++)
    {
        glBindTexture(GL_TEXTURE_2D, g_ogl.surface_tex_ids[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

        g_ogl.got_error = g_ogl.got_error || (err = glGetError()) != GL_NO_ERROR;
        
        if (err != GL_NO_ERROR)
        {
            TRACE("OpenGL error %08x (ogl_create_textures())\n", err);
            ogl_check_error("ogl_create_textures()");
        }

        while (glGetError() != GL_NO_ERROR);

        if (g_ddraw.bpp == 32)
        {
            glTexImage2D(
                GL_TEXTURE_2D,
                0,
                GL_RGBA8,
                g_ogl.surface_tex_width,
                g_ogl.surface_tex_height,
                0,
                g_ogl.surface_format = GL_BGRA,
                g_ogl.surface_type = GL_UNSIGNED_BYTE,
                0);
        }
        else if (g_ddraw.bpp == 16 && g_config.rgb555)
        {
            if (g_oglu_got_version3)
            {
                glTexImage2D(
                    GL_TEXTURE_2D,
                    0,
                    GL_RG8,
                    g_ogl.surface_tex_width,
                    g_ogl.surface_tex_height,
                    0,
                    g_ogl.surface_format = GL_RG,
                    g_ogl.surface_type = GL_UNSIGNED_BYTE,
                    0);
            }
            else
            {
                glTexImage2D(
                    GL_TEXTURE_2D,
                    0,
                    GL_RGBA8,
                    g_ogl.surface_tex_width,
                    g_ogl.surface_tex_height,
                    0,
                    g_ogl.surface_format = GL_BGRA,
                    g_ogl.surface_type = GL_UNSIGNED_SHORT_1_5_5_5_REV,
                    0);
            }
        }
        else if (g_ddraw.bpp == 16)
        {
            glTexImage2D(
                GL_TEXTURE_2D,
                0,
                GL_RGB565,
                g_ogl.surface_tex_width,
                g_ogl.surface_tex_height,
                0,
                g_ogl.surface_format = GL_RGB,
                g_ogl.surface_type = GL_UNSIGNED_SHORT_5_6_5,
                0);


            if (glGetError() != GL_NO_ERROR)
            {
                glTexImage2D(
                    GL_TEXTURE_2D,
                    0,
                    GL_RGB5,
                    g_ogl.surface_tex_width,
                    g_ogl.surface_tex_height,
                    0,
                    g_ogl.surface_format = GL_RGB,
                    g_ogl.surface_type = GL_UNSIGNED_SHORT_5_6_5,
                    0);
            }
        }
        else if (g_ddraw.bpp == 8)
        {
            glTexImage2D(
                GL_TEXTURE_2D,
                0,
                GL_LUMINANCE8,
                g_ogl.surface_tex_width,
                g_ogl.surface_tex_height,
                0,
                g_ogl.surface_format = GL_LUMINANCE,
                g_ogl.surface_type = GL_UNSIGNED_BYTE,
                0);


            if (glGetError() != GL_NO_ERROR)
            {
                glTexImage2D(
                    GL_TEXTURE_2D,
                    0,
                    GL_R8,
                    g_ogl.surface_tex_width,
                    g_ogl.surface_tex_height,
                    0,
                    g_ogl.surface_format = GL_RED,
                    g_ogl.surface_type = GL_UNSIGNED_BYTE,
                    0);
            }

            if (glGetError() != GL_NO_ERROR)
            {
                glTexImage2D(
                    GL_TEXTURE_2D,
                    0,
                    GL_RED,
                    g_ogl.surface_tex_width,
                    g_ogl.surface_tex_height,
                    0,
                    g_ogl.surface_format = GL_RED,
                    g_ogl.surface_type = GL_UNSIGNED_BYTE,
                    0);
            }
        }
    }

    if (g_ddraw.bpp == 8)
    {
        glGenTextures(TEXTURE_COUNT, g_ogl.palette_tex_ids);

        for (int i = 0; i < TEXTURE_COUNT; i++)
        {
            glBindTexture(GL_TEXTURE_2D, g_ogl.palette_tex_ids[i]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
        }
    }
}

static void ogl_init_main_program()
{
    if (!g_ogl.main_program)
        return;

    glUseProgram(g_ogl.main_program);

    glUniform1i(glGetUniformLocation(g_ogl.main_program, "Texture"), 0);

    if (g_ddraw.bpp == 8)
        glUniform1i(glGetUniformLocation(g_ogl.main_program, "PaletteTexture"), 1);

    if (g_oglu_got_version3)
    {
        g_ogl.main_vertex_coord_attr_loc = glGetAttribLocation(g_ogl.main_program, "VertexCoord");
        g_ogl.main_tex_coord_attr_loc = glGetAttribLocation(g_ogl.main_program, "TexCoord");

        glGenBuffers(3, g_ogl.main_vbos);

        if (g_ogl.shader_pass_count > 0)
        {
            glBindBuffer(GL_ARRAY_BUFFER, g_ogl.main_vbos[0]);
            static const GLfloat vertex_coord[] = {
                -1.0f,-1.0f,
                -1.0f, 1.0f,
                 1.0f, 1.0f,
                 1.0f,-1.0f,
            };
            glBufferData(GL_ARRAY_BUFFER, sizeof(vertex_coord), vertex_coord, GL_STATIC_DRAW);
            glBindBuffer(GL_ARRAY_BUFFER, 0);

            glBindBuffer(GL_ARRAY_BUFFER, g_ogl.main_vbos[1]);
            GLfloat tex_coord[] = {
                0.0f,          0.0f,
                0.0f,          g_ogl.scale_h,
                g_ogl.scale_w, g_ogl.scale_h,
                g_ogl.scale_w, 0.0f,
            };
            glBufferData(GL_ARRAY_BUFFER, sizeof(tex_coord), tex_coord, GL_STATIC_DRAW);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
        }
        else
        {
            glBindBuffer(GL_ARRAY_BUFFER, g_ogl.main_vbos[0]);
            static const GLfloat vertex_coord[] = {
                -1.0f, 1.0f,
                 1.0f, 1.0f,
                 1.0f,-1.0f,
                -1.0f,-1.0f,
            };
            glBufferData(GL_ARRAY_BUFFER, sizeof(vertex_coord), vertex_coord, GL_STATIC_DRAW);
            glBindBuffer(GL_ARRAY_BUFFER, 0);

            glBindBuffer(GL_ARRAY_BUFFER, g_ogl.main_vbos[1]);
            GLfloat tex_coord[] = {
                0.0f,          0.0f,
                g_ogl.scale_w, 0.0f,
                g_ogl.scale_w, g_ogl.scale_h,
                0.0f,          g_ogl.scale_h,
            };
            glBufferData(GL_ARRAY_BUFFER, sizeof(tex_coord), tex_coord, GL_STATIC_DRAW);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
        }

        glGenVertexArrays(1, &g_ogl.main_vao);
        glBindVertexArray(g_ogl.main_vao);

        glBindBuffer(GL_ARRAY_BUFFER, g_ogl.main_vbos[0]);
        glVertexAttribPointer(g_ogl.main_vertex_coord_attr_loc, 2, GL_FLOAT, GL_FALSE, 0, NULL);
        glEnableVertexAttribArray(g_ogl.main_vertex_coord_attr_loc);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        glBindBuffer(GL_ARRAY_BUFFER, g_ogl.main_vbos[1]);
        glVertexAttribPointer(g_ogl.main_tex_coord_attr_loc, 2, GL_FLOAT, GL_FALSE, 0, NULL);
        glEnableVertexAttribArray(g_ogl.main_tex_coord_attr_loc);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_ogl.main_vbos[2]);
        static const GLushort indices[] =
        {
            0, 1, 2,
            0, 2, 3,
        };
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

        glBindVertexArray(0);

        const float mvp_matrix[16] = {
            1,0,0,0,
            0,1,0,0,
            0,0,1,0,
            0,0,0,1,
        };
        glUniformMatrix4fv(glGetUniformLocation(g_ogl.main_program, "MVPMatrix"), 1, GL_FALSE, mvp_matrix);

    }
}

static void ogl_init_shader_programs()
{
    if (g_ogl.shader_pass_count <= 0)
        return;

    glGenFramebuffers(g_ogl.shader_pass_count, g_ogl.frame_buffer_id);
    glGenTextures(g_ogl.shader_pass_count, g_ogl.frame_buffer_tex_id);

    for (int i = 0; i < g_ogl.shader_pass_count; i++)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, g_ogl.frame_buffer_id[i]);

        glBindTexture(GL_TEXTURE_2D, g_ogl.frame_buffer_tex_id[i]);

        BOOL linear = g_ogl.shader_linear_filter[i] || (i == 0 && g_ogl.filter_bilinear);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            GL_RGBA8,
            g_ogl.surface_tex_width,
            g_ogl.surface_tex_height,
            0,
            GL_RGBA,
            GL_UNSIGNED_BYTE,
            0);

        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_ogl.frame_buffer_tex_id[i], 0);

        GLenum draw_buffers[1] = { GL_COLOR_ATTACHMENT0 };
        glDrawBuffers(1, draw_buffers);

        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        {
            glDeleteTextures(g_ogl.shader_pass_count, g_ogl.frame_buffer_tex_id);

            if (glDeleteFramebuffers)
                glDeleteFramebuffers(g_ogl.shader_pass_count, g_ogl.frame_buffer_id);

            g_ogl.shader_pass_count = 0;
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            return;
        }

        glClear(GL_COLOR_BUFFER_BIT);
    }

    for (int pass = 0; pass < g_ogl.shader_pass_count; pass++)
    {
        GLuint program = g_ogl.shader_programs[pass];
        glUseProgram(program);

        GLint vertex_coord_attr_loc = glGetAttribLocation(program, "VertexCoord");
        if (vertex_coord_attr_loc == -1)
            vertex_coord_attr_loc = glGetAttribLocation(program, "a_position");

        g_ogl.shader_tex_coord_attr_loc[pass] = glGetAttribLocation(program, "TexCoord");

        glGenBuffers(3, g_ogl.shader_vbos[pass]);

        const BOOL has_next_pass = pass < g_ogl.shader_pass_count - 1;

        glBindBuffer(GL_ARRAY_BUFFER, g_ogl.shader_vbos[pass][0]);
        if (has_next_pass)
        {
            static const GLfloat vertex_coord_flipped[] = {
                -1.0f,-1.0f,
                -1.0f, 1.0f,
                 1.0f, 1.0f,
                 1.0f,-1.0f,
            };
            glBufferData(GL_ARRAY_BUFFER, sizeof(vertex_coord_flipped), vertex_coord_flipped, GL_STATIC_DRAW);
        }
        else
        {
            static const GLfloat vertex_coord[] = {
                -1.0f, 1.0f,
                 1.0f, 1.0f,
                 1.0f,-1.0f,
                -1.0f,-1.0f,
            };
            glBufferData(GL_ARRAY_BUFFER, sizeof(vertex_coord), vertex_coord, GL_STATIC_DRAW);
        }
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        float scale_w = 0.0f;
        float scale_h = 0.0f;

        if (pass == 0)
        {
            scale_w = g_ogl.scale_w;
            scale_h = g_ogl.scale_h;
        }
        else
        {
            scale_w = g_ogl.shader_upscale[pass] ? g_ogl.scale_w : (float)g_ddraw.render.viewport.width / g_ogl.surface_tex_width;
            scale_h = g_ogl.shader_upscale[pass] ? g_ogl.scale_h : (float)g_ddraw.render.viewport.height / g_ogl.surface_tex_height;
        }

        glBindBuffer(GL_ARRAY_BUFFER, g_ogl.shader_vbos[pass][1]);
        GLfloat tex_coord[] = {
            0.0f,    0.0f,
            has_next_pass ? 0.0f : scale_w,    has_next_pass ? scale_h : 0.0f,
            scale_w, scale_h,
            has_next_pass ? scale_w : 0.0f,    has_next_pass ? 0.0f : scale_h,
        };
        glBufferData(GL_ARRAY_BUFFER, sizeof(tex_coord), tex_coord, GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        glGenVertexArrays(1, &g_ogl.shader_vao[pass]);
        glBindVertexArray(g_ogl.shader_vao[pass]);

        glBindBuffer(GL_ARRAY_BUFFER, g_ogl.shader_vbos[pass][0]);
        glVertexAttribPointer(vertex_coord_attr_loc, 2, GL_FLOAT, GL_FALSE, 0, NULL);
        glEnableVertexAttribArray(vertex_coord_attr_loc);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        if (g_ogl.shader_tex_coord_attr_loc[pass] != -1)
        {
            glBindBuffer(GL_ARRAY_BUFFER, g_ogl.shader_vbos[pass][1]);
            glVertexAttribPointer(g_ogl.shader_tex_coord_attr_loc[pass], 2, GL_FLOAT, GL_FALSE, 0, NULL);
            glEnableVertexAttribArray(g_ogl.shader_tex_coord_attr_loc[pass]);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
        }

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_ogl.shader_vbos[pass][2]);
        static const GLushort indices[] =
        {
            0, 1, 2,
            0, 2, 3,
        };
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

        glBindVertexArray(0);

        float input_size[2] = { 0 }, output_size[2] = { 0 }, texture_size[2] = { 0 };
        input_size[0] = (pass == 0 || g_ogl.shader_upscale[pass]) ? (float)g_ddraw.width : (float)g_ddraw.render.viewport.width;
        input_size[1] = (pass == 0 || g_ogl.shader_upscale[pass]) ? (float)g_ddraw.height : (float)g_ddraw.render.viewport.height;
        texture_size[0] = (float)g_ogl.surface_tex_width;
        texture_size[1] = (float)g_ogl.surface_tex_height;
        output_size[0] = (float)g_ddraw.render.viewport.width;
        output_size[1] = (float)g_ddraw.render.viewport.height;

        GLint loc = glGetUniformLocation(program, "OutputSize");
        if (loc == -1)
            loc = glGetUniformLocation(program, "rubyOutputSize");
        if (loc != -1)
            glUniform2fv(loc, 1, output_size);

        loc = glGetUniformLocation(program, "TextureSize");
        if (loc == -1)
            loc = glGetUniformLocation(program, "rubyTextureSize");
        if (loc != -1)
            glUniform2fv(loc, 1, texture_size);

        loc = glGetUniformLocation(program, "InputSize");
        if (loc == -1)
            loc = glGetUniformLocation(program, "rubyInputSize");
        if (loc != -1)
            glUniform2fv(loc, 1, input_size);

        loc = glGetUniformLocation(program, "Texture");
        if (loc == -1)
            loc = glGetUniformLocation(program, "rubyTexture");
        if (loc != -1)
            glUniform1i(loc, 0);

        loc = glGetUniformLocation(program, "OrigTexture");
        if (loc != -1)
            glUniform1i(loc, 1);

        loc = glGetUniformLocation(program, "OrigTextureSize");
        if (loc != -1)
            glUniform2fv(loc, 1, texture_size);

        for (int prev = 2; prev <= 7; prev++)
        {
            char uni[64] = { 0 };
            _snprintf(uni, sizeof(uni) - 1, "PassPrev%dTexture", prev);
            loc = glGetUniformLocation(program, uni);
            if (loc != -1)
                glUniform1i(loc, prev);

            _snprintf(uni, sizeof(uni) - 1, "PassPrev%dTextureSize", prev);
            loc = glGetUniformLocation(program, uni);
            if (loc != -1)
                glUniform2fv(loc, 1, texture_size);
        }

        loc = glGetUniformLocation(program, "FrameDirection");
        if (loc != -1)
            glUniform1i(loc, 1);

        g_ogl.shader_frame_count_uni_loc[pass] = glGetUniformLocation(program, "FrameCount");
        if (g_ogl.shader_frame_count_uni_loc[pass] == -1)
            g_ogl.shader_frame_count_uni_loc[pass] = glGetUniformLocation(program, "rubyFrameCount");

        const float mvp_matrix[16] = {
            1,0,0,0,
            0,1,0,0,
            0,0,1,0,
            0,0,0,1,
        };

        loc = glGetUniformLocation(program, "MVPMatrix");
        if (loc != -1)
            glUniformMatrix4fv(loc, 1, GL_FALSE, mvp_matrix);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void ogl_render()
{
    BOOL needs_update = FALSE;

    glViewport(
        g_ddraw.render.viewport.x, 
        g_ddraw.render.viewport.y + g_ddraw.render.opengl_y_align,
        g_ddraw.render.viewport.width, 
        g_ddraw.render.viewport.height);

    if (g_ogl.main_program)
    {
        glUseProgram(g_ogl.main_program);
    }
    else if (g_ddraw.bpp == 16 || g_ddraw.bpp == 32)
    {
        glEnable(GL_TEXTURE_2D);
    }
    else // 8 bpp only works with a shader (opengl 2.0 or above)
    {
        g_ogl.use_opengl = FALSE;
        return;
    }

    DWORD timeout = g_config.minfps > 0 ? g_ddraw.minfps_tick_len : INFINITE;

    while (g_ogl.use_opengl && g_ddraw.render.run &&
        (g_config.minfps < 0 || WaitForSingleObject(g_ddraw.render.sem, timeout) != WAIT_FAILED) &&
        g_ddraw.render.run)
    {
#if _DEBUG
        dbg_draw_frame_info_start();
#endif

        g_ogl.scale_w = (float)g_ddraw.width / g_ogl.surface_tex_width;
        g_ogl.scale_h = (float)g_ddraw.height / g_ogl.surface_tex_height;

        static int tex_index = 0, pal_index = 0;

        BOOL scale_changed = FALSE;

        fpsl_frame_start();

        EnterCriticalSection(&g_ddraw.cs);

        if (g_ddraw.primary && 
            g_ddraw.primary->bpp == g_ddraw.bpp &&
            g_ddraw.primary->width == g_ddraw.width &&
            g_ddraw.primary->height == g_ddraw.height &&
            (g_ddraw.bpp == 16 || g_ddraw.bpp == 32 || g_ddraw.primary->palette))
        {
            if (g_config.lock_surfaces)
                EnterCriticalSection(&g_ddraw.primary->cs);

            if (g_config.vhack)
            {
                if (util_detect_low_res_screen())
                {
                    g_ogl.scale_w *= (float)g_ddraw.upscale_hack_width / g_ddraw.width;
                    g_ogl.scale_h *= (float)g_ddraw.upscale_hack_height / g_ddraw.height;

                    if (!InterlockedExchange(&g_ddraw.upscale_hack_active, TRUE))
                        scale_changed = TRUE;
                }
                else
                {
                    if (InterlockedExchange(&g_ddraw.upscale_hack_active, FALSE))
                        scale_changed = TRUE;
                }
            }

            if (g_ddraw.bpp == 8 &&
                (InterlockedExchange(&g_ddraw.render.palette_updated, FALSE) || g_config.minfps == -2))
            {
                if (++pal_index >= TEXTURE_COUNT)
                    pal_index = 0;

                glBindTexture(GL_TEXTURE_2D, g_ogl.palette_tex_ids[pal_index]);

                glTexSubImage2D(
                    GL_TEXTURE_2D,
                    0,
                    0,
                    0,
                    256,
                    1,
                    GL_RGBA,
                    GL_UNSIGNED_BYTE,
                    g_ddraw.primary->palette->data_bgr);
            }

            if (InterlockedExchange(&g_ddraw.render.surface_updated, FALSE) || g_config.minfps == -2)
            {
                if (++tex_index >= TEXTURE_COUNT)
                    tex_index = 0;

                glBindTexture(GL_TEXTURE_2D, g_ogl.surface_tex_ids[tex_index]);

                DWORD row_len = g_ddraw.primary->pitch ? g_ddraw.primary->pitch / g_ddraw.primary->bytes_pp : 0;

                if (row_len != g_ddraw.primary->width)
                    glPixelStorei(GL_UNPACK_ROW_LENGTH, row_len);

                glTexSubImage2D(
                    GL_TEXTURE_2D,
                    0,
                    0,
                    0,
                    g_ddraw.width,
                    g_ddraw.height,
                    g_ogl.surface_format,
                    g_ogl.surface_type,
                    g_ddraw.primary->surface);

                if (row_len != g_ddraw.primary->width)
                    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            }

            static int error_check_count = 0;

            if (error_check_count < 10)
            {
                error_check_count++;

                GLenum err = glGetError();

                if (err != GL_NO_ERROR && err != GL_INVALID_FRAMEBUFFER_OPERATION)
                {
                    g_ogl.use_opengl = FALSE;

                    TRACE("OpenGL error %08x (ogl_render())\n", err);
                    ogl_check_error("ogl_render()");
                }
            }

            if (g_config.fixchilds)
            {
                g_ddraw.child_window_exists = FALSE;
                EnumChildWindows(g_ddraw.hwnd, util_enum_child_proc, (LPARAM)g_ddraw.primary);

                if (g_ddraw.render.width != g_ddraw.width || g_ddraw.render.height != g_ddraw.height)
                {
                    if (g_ddraw.child_window_exists)
                    {
                        glClear(GL_COLOR_BUFFER_BIT);

                        if (!needs_update)
                        {
                            glViewport(0, g_ddraw.render.height - g_ddraw.height, g_ddraw.width, g_ddraw.height);
                            needs_update = TRUE;
                        }
                    }
                    else if (needs_update)
                    {
                        glViewport(
                            g_ddraw.render.viewport.x, 
                            g_ddraw.render.viewport.y + g_ddraw.render.opengl_y_align,
                            g_ddraw.render.viewport.width, 
                            g_ddraw.render.viewport.height);

                        needs_update = FALSE;
                    }
                }
            }

            if (g_config.lock_surfaces)
                LeaveCriticalSection(&g_ddraw.primary->cs);
        }

        LeaveCriticalSection(&g_ddraw.cs);

        if (g_ddraw.render.viewport.x != 0 || g_ddraw.render.viewport.y != 0)
        {
            glClear(GL_COLOR_BUFFER_BIT);
        }

        if (scale_changed)
        {
            if (g_ogl.shader_pass_count > 0 && g_ogl.main_program)
            {
                for (int pass = 0; pass < g_ogl.shader_pass_count; pass++)
                {
                    const BOOL has_next_pass = pass < g_ogl.shader_pass_count - 1;
                    float scale_w = pass == 0 ? g_ogl.scale_w :
                        (g_ogl.shader_upscale[pass] ? g_ogl.scale_w : (float)g_ddraw.render.viewport.width / g_ogl.surface_tex_width);
                    float scale_h = pass == 0 ? g_ogl.scale_h :
                        (g_ogl.shader_upscale[pass] ? g_ogl.scale_h : (float)g_ddraw.render.viewport.height / g_ogl.surface_tex_height);

                    glBindVertexArray(g_ogl.shader_vao[pass]);
                    glBindBuffer(GL_ARRAY_BUFFER, g_ogl.shader_vbos[pass][1]);
                    GLfloat tex_coord[] = {
                        0.0f,    0.0f,
                        has_next_pass ? 0.0f : scale_w,    has_next_pass ? scale_h : 0.0f,
                        scale_w, scale_h,
                        has_next_pass ? scale_w : 0.0f,    has_next_pass ? 0.0f : scale_h,
                    };
                    glBufferData(GL_ARRAY_BUFFER, sizeof(tex_coord), tex_coord, GL_STATIC_DRAW);
                    if (g_ogl.shader_tex_coord_attr_loc[pass] != -1)
                    {
                        glVertexAttribPointer(g_ogl.shader_tex_coord_attr_loc[pass], 2, GL_FLOAT, GL_FALSE, 0, NULL);
                        glEnableVertexAttribArray(g_ogl.shader_tex_coord_attr_loc[pass]);
                    }
                    glBindBuffer(GL_ARRAY_BUFFER, 0);
                    glBindVertexArray(0);
                }
            }
            else if (g_oglu_got_version3 && g_ogl.main_program)
            {
                glBindVertexArray(g_ogl.main_vao);
                glBindBuffer(GL_ARRAY_BUFFER, g_ogl.main_vbos[1]);
                GLfloat tex_coord[] = {
                    0.0f,           0.0f,
                    g_ogl.scale_w,  0.0f,
                    g_ogl.scale_w,  g_ogl.scale_h,
                    0.0f,           g_ogl.scale_h,
                };
                glBufferData(GL_ARRAY_BUFFER, sizeof(tex_coord), tex_coord, GL_STATIC_DRAW);
                glVertexAttribPointer(g_ogl.main_tex_coord_attr_loc, 2, GL_FLOAT, GL_FALSE, 0, NULL);
                glEnableVertexAttribArray(g_ogl.main_tex_coord_attr_loc);
                glBindBuffer(GL_ARRAY_BUFFER, 0);
                glBindVertexArray(0);
            }
        }

        if (glActiveTexture)
            glActiveTexture(GL_TEXTURE0);

        glBindTexture(GL_TEXTURE_2D, g_ogl.surface_tex_ids[tex_index]);

        if (g_ddraw.bpp == 8)
        {
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, g_ogl.palette_tex_ids[pal_index]);

            glActiveTexture(GL_TEXTURE0);
        }

        if (g_ogl.shader_pass_count > 0 && g_ogl.main_program)
        {
            static int frames = 0;
            frames++;

            /* draw surface into framebuffer */
            glUseProgram(g_ogl.main_program);

            glViewport(0, 0, g_ddraw.width, g_ddraw.height);

            glBindFramebuffer(GL_FRAMEBUFFER, g_ogl.frame_buffer_id[0]);

            glBindVertexArray(g_ogl.main_vao);
            glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
            glBindVertexArray(0);

            glBindFramebuffer(GL_FRAMEBUFFER, 0);

            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, 0);

            int source_tex_index = 0;

            for (int pass = 0; pass < g_ogl.shader_pass_count; pass++)
            {
                BOOL final_pass = pass == g_ogl.shader_pass_count - 1;

                if (final_pass)
                {
                    if (g_ddraw.child_window_exists)
                    {
                        glViewport(0, g_ddraw.render.height - g_ddraw.height, g_ddraw.width, g_ddraw.height);
                    }
                    else
                    {
                        glViewport(
                            g_ddraw.render.viewport.x,
                            g_ddraw.render.viewport.y + g_ddraw.render.opengl_y_align,
                            g_ddraw.render.viewport.width,
                            g_ddraw.render.viewport.height);
                    }

                    glBindFramebuffer(GL_FRAMEBUFFER, 0);
                }
                else
                {
                    if (pass == 0 || g_ogl.shader_upscale[pass])
                    {
                        glViewport(0, 0, g_ddraw.width, g_ddraw.height);
                    }
                    else
                    {
                        glViewport(0, 0, g_ddraw.render.viewport.width, g_ddraw.render.viewport.height);
                    }

                    glBindFramebuffer(GL_FRAMEBUFFER, g_ogl.frame_buffer_id[pass + 1]);
                }

                glUseProgram(g_ogl.shader_programs[pass]);

                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, g_ogl.frame_buffer_tex_id[source_tex_index]);

                glActiveTexture(GL_TEXTURE1);
                glBindTexture(GL_TEXTURE_2D, g_ogl.surface_tex_ids[tex_index]);

                for (int prev = 2; prev <= 7; prev++)
                {
                    int prev_index = pass - (prev - 1);
                    if (prev_index < 0)
                        prev_index = 0;

                    glActiveTexture(GL_TEXTURE0 + prev);
                    glBindTexture(GL_TEXTURE_2D, g_ogl.frame_buffer_tex_id[prev_index]);
                }

                if (g_ogl.shader_frame_count_uni_loc[pass] != -1)
                    glUniform1i(g_ogl.shader_frame_count_uni_loc[pass], frames);

                glBindVertexArray(g_ogl.shader_vao[pass]);
                glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
                glBindVertexArray(0);

                if (!final_pass)
                    source_tex_index = pass + 1;
            }

            for (int i = 1; i <= 7; i++)
            {
                glActiveTexture(GL_TEXTURE0 + i);
                glBindTexture(GL_TEXTURE_2D, 0);
            }

            glActiveTexture(GL_TEXTURE0);
        }
        else if (g_oglu_got_version3 && g_ogl.main_program)
        {
            glBindVertexArray(g_ogl.main_vao);
            glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
            glBindVertexArray(0);
        }
        else
        {
            glBegin(GL_TRIANGLE_FAN);
            glTexCoord2f(0, 0);                          glVertex2f(-1, 1);
            glTexCoord2f(g_ogl.scale_w, 0);              glVertex2f(1, 1);
            glTexCoord2f(g_ogl.scale_w, g_ogl.scale_h);  glVertex2f(1, -1);
            glTexCoord2f(0, g_ogl.scale_h);              glVertex2f(-1, -1);
            glEnd();
        }

        if (g_ddraw.bnet_active)
            glClear(GL_COLOR_BUFFER_BIT);

        SwapBuffers(g_ogl.hdc);

        /* Force redraw for GDI games (ClueFinders) */
        if (!g_ddraw.primary)
        {
            RedrawWindow(g_ddraw.hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN);
        }

        if (!g_ddraw.render.run)
            break;

#if _DEBUG
        dbg_draw_frame_info_end();
#endif

        fpsl_frame_end();
    }

    if (g_config.vhack)
        InterlockedExchange(&g_ddraw.upscale_hack_active, FALSE);
}

static BOOL ogl_release_resources()
{
    glDeleteTextures(TEXTURE_COUNT, g_ogl.surface_tex_ids);

    if (g_ddraw.bpp == 8)
        glDeleteTextures(TEXTURE_COUNT, g_ogl.palette_tex_ids);

    if (glUseProgram)
        glUseProgram(0);

    if (g_ogl.shader_pass_count > 0)
    {
        glDeleteTextures(g_ogl.shader_pass_count, g_ogl.frame_buffer_tex_id);

        if (glDeleteFramebuffers)
            glDeleteFramebuffers(g_ogl.shader_pass_count, g_ogl.frame_buffer_id);

        for (int pass = 0; pass < g_ogl.shader_pass_count; pass++)
        {
            if (glDeleteBuffers)
                glDeleteBuffers(3, g_ogl.shader_vbos[pass]);

            if (glDeleteVertexArrays)
                glDeleteVertexArrays(1, &g_ogl.shader_vao[pass]);
        }
    }

    if (glDeleteProgram)
    {
        if (g_ogl.main_program)
            glDeleteProgram(g_ogl.main_program);

        for (int pass = 0; pass < g_ogl.shader_pass_count; pass++)
        {
            if (g_ogl.shader_programs[pass])
                glDeleteProgram(g_ogl.shader_programs[pass]);
        }
    }

    if (g_oglu_got_version3)
    {
        if (g_ogl.main_program)
        {
            if (glDeleteBuffers)
                glDeleteBuffers(3, g_ogl.main_vbos);

            if (glDeleteVertexArrays)
                glDeleteVertexArrays(1, &g_ogl.main_vao);
        }
    }

    return TRUE;
}

BOOL ogl_release()
{
    if (g_ddraw.render.thread)
        return FALSE;

    if (g_ogl.context)
    {
        xwglMakeCurrent(NULL, NULL);
        xwglDeleteContext(g_ogl.context);
        g_ogl.context = NULL;
    }

    return TRUE;
}

static BOOL ogl_texture_upload_test()
{
    if (g_ogl.surface_tex_width > 4096 || g_ogl.surface_tex_height > 4096)
        return TRUE;

    int* surface_tex =
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, g_ogl.surface_tex_width * g_ogl.surface_tex_height * sizeof(int));

    if (!surface_tex)
        return TRUE;

    static char test_data[] = { 0,1,2,0,0,2,3,0,0,4,5,0,0,6,7,0,0,8,9,0 };

    int i;
    for (i = 0; i < TEXTURE_COUNT; i++)
    {
        memcpy(surface_tex, test_data, sizeof(test_data));

        glBindTexture(GL_TEXTURE_2D, g_ogl.surface_tex_ids[i]);

        glTexSubImage2D(
            GL_TEXTURE_2D,
            0,
            0,
            0,
            g_ddraw.width,
            g_ddraw.height,
            g_ogl.surface_format,
            g_ogl.surface_type,
            surface_tex);

        memset(surface_tex, 0, sizeof(test_data));

        glGetTexImage(GL_TEXTURE_2D, 0, g_ogl.surface_format, g_ogl.surface_type, surface_tex);

        if (memcmp(surface_tex, test_data, sizeof(test_data)) != 0)
        {
            HeapFree(GetProcessHeap(), 0, surface_tex);
            return FALSE;
        }
    }

    if (g_ddraw.bpp == 8)
    {
        for (i = 0; i < TEXTURE_COUNT; i++)
        {
            glBindTexture(GL_TEXTURE_2D, g_ogl.palette_tex_ids[i]);

            glTexSubImage2D(
                GL_TEXTURE_2D,
                0,
                0,
                0,
                256,
                1,
                GL_RGBA,
                GL_UNSIGNED_BYTE,
                surface_tex);

            memset(surface_tex, 0, sizeof(test_data));

            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, surface_tex);

            if (memcmp(surface_tex, test_data, sizeof(test_data)) != 0)
            {
                HeapFree(GetProcessHeap(), 0, surface_tex);
                return FALSE;
            }
                
        }
    }

    HeapFree(GetProcessHeap(), 0, surface_tex);
    return TRUE;
}

static BOOL ogl_shader_test()
{
    BOOL result = TRUE;

    if (g_ddraw.bpp != 8 || g_ogl.surface_tex_width > 4096 || g_ogl.surface_tex_height > 4096)
        return result;

    int* surface_tex =
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, g_ogl.surface_tex_width * g_ogl.surface_tex_height * sizeof(int));

    if (!surface_tex)
        return TRUE;

    if (g_oglu_got_version3 && g_ogl.main_program)
    {
        memset(surface_tex, 0, g_ogl.surface_tex_height * g_ogl.surface_tex_width * sizeof(int));

        GLuint fbo_id = 0;
        glGenFramebuffers(1, &fbo_id);

        glBindFramebuffer(GL_FRAMEBUFFER, fbo_id);

        GLuint fbo_tex_id = 0;
        glGenTextures(1, &fbo_tex_id);
        glBindTexture(GL_TEXTURE_2D, fbo_tex_id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            GL_RGBA8,
            g_ogl.surface_tex_width,
            g_ogl.surface_tex_height,
            0,
            GL_RGBA,
            GL_UNSIGNED_BYTE,
            surface_tex);

        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fbo_tex_id, 0);

        GLenum draw_buffers[1] = { GL_COLOR_ATTACHMENT0 };
        glDrawBuffers(1, draw_buffers);

        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE)
        {
            static char gray0pal[] = { 128,128,128,128 };

            glBindTexture(GL_TEXTURE_2D, g_ogl.palette_tex_ids[0]);

            glTexSubImage2D(
                GL_TEXTURE_2D,
                0,
                0,
                0,
                1,
                1,
                GL_RGBA,
                GL_UNSIGNED_BYTE,
                gray0pal);

            glBindTexture(GL_TEXTURE_2D, g_ogl.surface_tex_ids[0]);

            glTexSubImage2D(
                GL_TEXTURE_2D,
                0,
                0,
                0,
                g_ogl.surface_tex_width,
                g_ogl.surface_tex_height,
                g_ogl.surface_format,
                GL_UNSIGNED_BYTE,
                surface_tex);

            glViewport(0, 0, g_ogl.surface_tex_width, g_ogl.surface_tex_height);

            glUseProgram(g_ogl.main_program);

            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, g_ogl.palette_tex_ids[0]);
            glActiveTexture(GL_TEXTURE0);

            glBindVertexArray(g_ogl.main_vao);
            glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
            glBindVertexArray(0);

            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, 0);
            glActiveTexture(GL_TEXTURE0);

            glBindTexture(GL_TEXTURE_2D, fbo_tex_id);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, surface_tex);

            if (glGetError() == GL_NO_ERROR)
            {
                int i;
                for (i = 0; i < g_ogl.surface_tex_height * g_ogl.surface_tex_width; i++)
                {
                    if (surface_tex[i] != 0x80808080)
                    {
                        result = FALSE;
                        break;
                    }
                }
            }
        }

        glBindTexture(GL_TEXTURE_2D, 0);

        if (glDeleteFramebuffers)
            glDeleteFramebuffers(1, &fbo_id);

        glDeleteTextures(1, &fbo_tex_id);

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    while (glGetError() != GL_NO_ERROR);

    HeapFree(GetProcessHeap(), 0, surface_tex);
    return result;
}
