#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>

#include <SDL2/SDL.h>
#define GLEW_STATIC
#include <GL/glew.h>
#define GL_GLEXT_PROTOTYPES
#include <SDL2/SDL_opengl.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include "./editor.h"
#include "./file_browser.h"
#include "./la.h"
#include "./free_glyph.h"
#include "./simple_renderer.h"
#include "./common.h"

#define SCREEN_WIDTH 800
#define SCREEN_HEIGHT 600
#define FPS 60
#define DELTA_TIME (1.0f / FPS)

void usage(FILE *stream)
{
    fprintf(stream, "Usage: te [FILE-PATH]\n");
}

// TODO: Save file dialog
// Needed when ded is ran without any file so it does not know where to save.
// TODO: File Manager
// Any modern Text Editor should also be a File Manager

// TODO: Jump forward/backward by a word
// TODO: Delete a word

void MessageCallback(GLenum source,
                     GLenum type,
                     GLuint id,
                     GLenum severity,
                     GLsizei length,
                     const GLchar* message,
                     const void* userParam)
{
    (void) source;
    (void) id;
    (void) length;
    (void) userParam;
    fprintf(stderr, "GL CALLBACK: %s type = 0x%x, severity = 0x%x, message = %s\n",
            (type == GL_DEBUG_TYPE_ERROR ? "** GL ERROR **" : ""),
            type, severity, message);
}

static Free_Glyph_Atlas atlas = {0};
static Simple_Renderer sr = {0};
static Editor editor = {0};
static File_Browser fb = {0};
static Uint32 last_stroke = 0;

#define FREE_GLYPH_FONT_SIZE 64
#define ZOOM_OUT_GLYPH_THRESHOLD 30

void render_file_browser(SDL_Window *window, Free_Glyph_Atlas *atlas, Simple_Renderer *sr, const File_Browser *fb)
{
    Vec2f cursor_pos = vec2f(0, -(float)fb->cursor * FREE_GLYPH_FONT_SIZE);

    int w, h;
    SDL_GetWindowSize(window, &w, &h);

    float max_line_len = 0.0f;

    sr->resolution = vec2f(w, h);
    sr->time = (float) SDL_GetTicks() / 1000.0f;

    simple_renderer_set_shader(sr, SHADER_FOR_COLOR);
    if (fb->cursor < fb->files.count) {
        const Vec2f begin = vec2f(0, -(float)fb->cursor * FREE_GLYPH_FONT_SIZE);
        Vec2f end = begin;
        free_glyph_atlas_render_line_sized(
            atlas, sr, fb->files.items[fb->cursor], strlen(fb->files.items[fb->cursor]),
            &end,
            false);
        simple_renderer_solid_rect(sr, begin, vec2f(end.x - begin.x, FREE_GLYPH_FONT_SIZE), vec4f(.25, .25, .25, 1));
    }
    simple_renderer_flush(sr);

    simple_renderer_set_shader(sr, SHADER_FOR_EPICNESS);
    for (size_t row = 0; row < fb->files.count; ++row) {
        const Vec2f begin = vec2f(0, -(float)row * FREE_GLYPH_FONT_SIZE);
        Vec2f end = begin;
        free_glyph_atlas_render_line_sized(
            atlas, sr, fb->files.items[row], strlen(fb->files.items[row]),
            &end,
            true);
        // TODO: the max_line_len should be calculated based on what's visible on the screen right now
        float line_len = fabsf(end.x - begin.x);
        if (line_len > max_line_len) {
            max_line_len = line_len;
        }
    }

    simple_renderer_flush(sr);

    // Update camera
    {
        float target_scale = 3.0f;
        if (max_line_len > 0.0f) {
            target_scale = SCREEN_WIDTH / max_line_len;
        }

        if (target_scale > 3.0f) {
            target_scale = 3.0f;
        }


        sr->camera_vel = vec2f_mul(
                             vec2f_sub(cursor_pos, sr->camera_pos),
                             vec2fs(2.0f));
        sr->camera_scale_vel = (target_scale - sr->camera_scale) * 2.0f;

        sr->camera_pos = vec2f_add(sr->camera_pos, vec2f_mul(sr->camera_vel, vec2fs(DELTA_TIME)));
        sr->camera_scale = sr->camera_scale + sr->camera_scale_vel * DELTA_TIME;
    }
}

void render_editor(SDL_Window *window, Free_Glyph_Atlas *atlas, Simple_Renderer *sr, Editor *editor)
{
    int w, h;
    SDL_GetWindowSize(window, &w, &h);

    float max_line_len = 0.0f;

    sr->resolution = vec2f(w, h);
    sr->time = (float) SDL_GetTicks() / 1000.0f;

    // Render text
    simple_renderer_set_shader(sr, SHADER_FOR_EPICNESS);
    {
        for (size_t row = 0; row < editor->lines.count; ++row) {
            Line line = editor->lines.items[row];

            const Vec2f begin = vec2f(0, -(float)row * FREE_GLYPH_FONT_SIZE);
            Vec2f end = begin;
            free_glyph_atlas_render_line_sized(
                atlas, sr, editor->data.items + line.begin, line.end - line.begin,
                &end,
                true);
            // TODO: the max_line_len should be calculated based on what's visible on the screen right now
            float line_len = fabsf(end.x - begin.x);
            if (line_len > max_line_len) {
                max_line_len = line_len;
            }
        }

        simple_renderer_flush(sr);
    }

    Vec2f cursor_pos = vec2fs(0.0f);
    {
        size_t cursor_row = editor_cursor_row(editor);
        Line line = editor->lines.items[cursor_row];
        size_t cursor_col = editor->cursor - line.begin;
        cursor_pos.y = -(float) cursor_row * FREE_GLYPH_FONT_SIZE;
        cursor_pos.x = free_glyph_atlas_cursor_pos(
                           atlas,
                           editor->data.items + line.begin, line.end - line.begin,
                           vec2f(0.0, cursor_pos.y),
                           cursor_col
                       );
    }

    // Render cursor
    simple_renderer_set_shader(sr, SHADER_FOR_COLOR);
    {
        float CURSOR_WIDTH = 5.0f;
        Uint32 CURSOR_BLINK_THRESHOLD = 500;
        Uint32 CURSOR_BLINK_PERIOD = 1000;
        Uint32 t = SDL_GetTicks() - last_stroke;

        sr->verticies_count = 0;
        if (t < CURSOR_BLINK_THRESHOLD || t/CURSOR_BLINK_PERIOD%2 != 0) {
            simple_renderer_solid_rect(
                sr,
                cursor_pos, vec2f(CURSOR_WIDTH, FREE_GLYPH_FONT_SIZE),
                vec4fs(1));
        }

        simple_renderer_flush(sr);
    }

    // Update camera
    {
        float target_scale = 3.0f;
        if (max_line_len > 1000.0f) {
            max_line_len = 1000.0f;
        }
        if (max_line_len > 0.0f) {
            target_scale = SCREEN_WIDTH / max_line_len;
        }

        if (target_scale > 3.0f) {
            target_scale = 3.0f;
        }

        sr->camera_vel = vec2f_mul(
                             vec2f_sub(cursor_pos, sr->camera_pos),
                             vec2fs(2.0f));
        sr->camera_scale_vel = (target_scale - sr->camera_scale) * 2.0f;

        sr->camera_pos = vec2f_add(sr->camera_pos, vec2f_mul(sr->camera_vel, vec2fs(DELTA_TIME)));
        sr->camera_scale = sr->camera_scale + sr->camera_scale_vel * DELTA_TIME;
    }
}

// TODO: display errors reported via flash_error right in the text editor window somehow
#define flash_error(...) fprintf(stderr, __VA_ARGS__)

int main(int argc, char **argv)
{
    Errno err;

    editor_recompute_lines(&editor);

    FT_Library library = {0};

    FT_Error error = FT_Init_FreeType(&library);
    if (error) {
        fprintf(stderr, "ERROR: could initialize FreeType2 library\n");
        return 1;
    }

    const char *const font_file_path = "./VictorMono-Regular.ttf";

    FT_Face face;
    error = FT_New_Face(library, font_file_path, 0, &face);
    if (error == FT_Err_Unknown_File_Format) {
        fprintf(stderr, "ERROR: `%s` has an unknown format\n", font_file_path);
        return 1;
    } else if (error) {
        fprintf(stderr, "ERROR: could not load file `%s`\n", font_file_path);
        return 1;
    }

    FT_UInt pixel_size = FREE_GLYPH_FONT_SIZE;
    // TODO: FT_Set_Pixel_Sizes does not produce good looking results
    // We need to use something like FT_Set_Char_Size and properly set the device resolution
    error = FT_Set_Pixel_Sizes(face, 0, pixel_size);
    if (error) {
        fprintf(stderr, "ERROR: could not set pixel size to %u\n", pixel_size);
        return 1;
    }


    if (argc > 1) {
        const char *file_path = argv[1];
        err = editor_load_from_file(&editor, file_path);
        if (err != 0) {
            fprintf(stderr, "ERROR: Could ont read file %s: %s\n", file_path, strerror(err));
            return 1;
        }
    }

    const char *dir_path = ".";
    err = fb_open_dir(&fb, dir_path);
    if (err != 0) {
        fprintf(stderr, "ERROR: Could not read directory %s: %s\n", dir_path, strerror(err));
        return 1;
    }

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "ERROR: Could not initialize SDL: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window *window =
        SDL_CreateWindow("Text Editor",
                         0, 0,
                         SCREEN_WIDTH, SCREEN_HEIGHT,
                         SDL_WINDOW_RESIZABLE | SDL_WINDOW_OPENGL);
    if (window == NULL) {
        fprintf(stderr, "ERROR: Could not create SDL window: %s\n", SDL_GetError());
        return 1;
    }

    {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);

        int major;
        int minor;
        SDL_GL_GetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, &major);
        SDL_GL_GetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, &minor);
        printf("GL version %d.%d\n", major, minor);
    }

    if (SDL_GL_CreateContext(window) == NULL) {
        fprintf(stderr, "Could not create OpenGL context: %s\n", SDL_GetError());
        return 1;
    }

    if (GLEW_OK != glewInit()) {
        fprintf(stderr, "Could not initialize GLEW!");
        return 1;
    }

    if (!GLEW_ARB_draw_instanced) {
        fprintf(stderr, "ARB_draw_instanced is not supported; game may not work properly!!\n");
        return 1;
    }

    if (!GLEW_ARB_instanced_arrays) {
        fprintf(stderr, "ARB_instanced_arrays is not supported; game may not work properly!!\n");
        return 1;
    }

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    if (GLEW_ARB_debug_output) {
        glEnable(GL_DEBUG_OUTPUT);
        glDebugMessageCallback(MessageCallback, 0);
    } else {
        fprintf(stderr, "WARNING! GLEW_ARB_debug_output is not available");
    }


    simple_renderer_init(&sr,
                         "./shaders/simple.vert",
                         "./shaders/simple_color.frag",
                         "./shaders/simple_image.frag",
                         "./shaders/simple_epic.frag");
    free_glyph_atlas_init(&atlas, face);

    bool quit = false;
    bool file_browser = false;
    while (!quit) {
        const Uint32 start = SDL_GetTicks();
        SDL_Event event = {0};
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_QUIT: {
                quit = true;
            }
            break;

            case SDL_KEYDOWN: {
                if (file_browser) {
                    switch (event.key.keysym.sym) {
                    case SDLK_F3: {
                        file_browser = false;
                    }
                    break;

                    case SDLK_UP: {
                        if (fb.cursor > 0) fb.cursor -= 1;
                    }
                    break;

                    case SDLK_DOWN: {
                        if (fb.cursor + 1 < fb.files.count) fb.cursor += 1;
                    }
                    break;

                    case SDLK_RETURN: {
                        if (fb.cursor < fb.files.count) {
                            // TODO: go into folders
                            const char *file_path = fb.files.items[fb.cursor];
                            err = editor_load_from_file(&editor, file_path);
                            if (err != 0) {
                                flash_error("Could not open file %s: %s", file_path, strerror(err));
                            } else {
                                file_browser = false;
                            }
                        }
                    }
                    break;
                    }
                } else {
                    switch (event.key.keysym.sym) {
                    case SDLK_BACKSPACE: {
                        editor_backspace(&editor);
                        last_stroke = SDL_GetTicks();
                    }
                    break;

                    case SDLK_F2: {
                        if (editor.file_path.count > 0) {
                            err = editor_save(&editor);
                            if (err != 0) {
                                flash_error("Could not save file currently edited file: %s", strerror(err));
                            }
                        } else {
                            // TODO: as the user for the path to save to in this situation
                            flash_error("No where to save the text");
                        }
                    }
                    break;

                    case SDLK_F3: {
                        file_browser = true;
                    }
                    break;

                    case SDLK_RETURN: {
                        editor_insert_char(&editor, '\n');
                        last_stroke = SDL_GetTicks();
                    }
                    break;

                    case SDLK_DELETE: {
                        editor_delete(&editor);
                        last_stroke = SDL_GetTicks();
                    }
                    break;

                    case SDLK_UP: {
                        editor_move_line_up(&editor);
                        last_stroke = SDL_GetTicks();
                    }
                    break;

                    case SDLK_DOWN: {
                        editor_move_line_down(&editor);
                        last_stroke = SDL_GetTicks();
                    }
                    break;

                    case SDLK_LEFT: {
                        editor_move_char_left(&editor);
                        last_stroke = SDL_GetTicks();
                    }
                    break;

                    case SDLK_RIGHT: {
                        editor_move_char_right(&editor);
                        last_stroke = SDL_GetTicks();
                    }
                    break;
                    }
                }
            }
            break;

            case SDL_TEXTINPUT: {
                if (file_browser) {
                    // TODO: file browser keys
                } else {
                    const char *text = event.text.text;
                    size_t text_len = strlen(text);
                    for (size_t i = 0; i < text_len; ++i) {
                        editor_insert_char(&editor, text[i]);
                    }
                    last_stroke = SDL_GetTicks();
                }
            }
            break;
            }
        }

        {
            int w, h;
            SDL_GetWindowSize(window, &w, &h);
            // TODO(#19): update the viewport and the resolution only on actual window change
            glViewport(0, 0, w, h);
        }

        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        if (file_browser) {
            render_file_browser(window, &atlas, &sr, &fb);
        } else {
            render_editor(window, &atlas, &sr, &editor);
        }

        SDL_GL_SwapWindow(window);

        const Uint32 duration = SDL_GetTicks() - start;
        const Uint32 delta_time_ms = 1000 / FPS;
        if (duration < delta_time_ms) {
            SDL_Delay(delta_time_ms - duration);
        }
    }

    return 0;
}
