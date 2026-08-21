/* overlay.c -- lightweight on-screen FPS/resolution overlay
 *
 * Copyright (C) 2026 Andrii Romaniuk
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdio.h>
#include <string.h>
#include <switch.h>
#include <GLES2/gl2.h>

#include "config.h"
#include "overlay.h"

// Number of swaps to let go by before the overlay ever touches GL state.
// Keeps it well clear of the loading screens / initial shader compiles seen
// in the eglSwapBuffers_wrapper debug log (call attempts to bind and reset
// GL state on every swap, so we simply wait until things have settled).
#define OVERLAY_WARMUP_SWAPS 180

// Screen-pixel size of a single font dot and the gap between characters.
#define GLYPH_SCALE 3
#define GLYPH_COLS 5
#define GLYPH_ROWS 7
#define GLYPH_SPACING 2
#define OVERLAY_MARGIN 10
#define OVERLAY_PAD 6

extern int diag_get_egl_swap_count(void);

typedef struct
{
  char ch;
  unsigned char rows[GLYPH_ROWS]; // top->bottom, bit4 = leftmost column
} Glyph;

static const Glyph k_font[] = {
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
    {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
    {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
    {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
    {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
    {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    {':', {0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00}},
    {'x', {0x00, 0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11}},
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
};

static const Glyph *glyph_lookup(char c)
{
  for (unsigned i = 0; i < sizeof(k_font) / sizeof(k_font[0]); ++i)
    if (k_font[i].ch == c)
      return &k_font[i];
  return &k_font[sizeof(k_font) / sizeof(k_font[0]) - 1]; // space
}

// Vertex buffer big enough for a "999 FPS  9999x9999" sized string plus the
// background panel; each glyph pixel becomes one 2-triangle quad.
#define OVERLAY_MAX_CHARS 24
#define OVERLAY_MAX_VERTS ((OVERLAY_MAX_CHARS * GLYPH_COLS * GLYPH_ROWS + 1) * 6)

static float s_verts[OVERLAY_MAX_VERTS * 2];

static GLuint s_program = 0;
static GLuint s_vbo = 0;
static GLint s_uColor = -1;
static GLint s_aPos = -1;
static int s_gl_ready = 0;

static const char *k_vs_src =
    "attribute vec2 aPos;\n"
    "void main() { gl_Position = vec4(aPos, 0.0, 1.0); }\n";

static const char *k_fs_src =
    "precision mediump float;\n"
    "uniform vec4 uColor;\n"
    "void main() { gl_FragColor = uColor; }\n";

static GLuint compile_shader(GLenum type, const char *src)
{
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  return s;
}

static int overlay_gl_init(void)
{
  GLuint vs = compile_shader(GL_VERTEX_SHADER, k_vs_src);
  GLuint fs = compile_shader(GL_FRAGMENT_SHADER, k_fs_src);

  s_program = glCreateProgram();
  glAttachShader(s_program, vs);
  glAttachShader(s_program, fs);
  glBindAttribLocation(s_program, 0, "aPos");
  glLinkProgram(s_program);

  GLint linked = 0;
  glGetProgramiv(s_program, GL_LINK_STATUS, &linked);

  glDeleteShader(vs);
  glDeleteShader(fs);

  if (!linked)
  {
    glDeleteProgram(s_program);
    s_program = 0;
    return 0;
  }

  s_uColor = glGetUniformLocation(s_program, "uColor");
  s_aPos = glGetAttribLocation(s_program, "aPos");

  glGenBuffers(1, &s_vbo);
  return 1;
}

// Appends one screen-space pixel quad (top-left origin) as two triangles,
// converted straight to NDC using the current screen dimensions.
static float *push_pixel_quad(float *out, float px, float py, float pw, float ph)
{
  const float x0 = (px / (float)screen_width) * 2.0f - 1.0f;
  const float x1 = ((px + pw) / (float)screen_width) * 2.0f - 1.0f;
  const float y0 = 1.0f - (py / (float)screen_height) * 2.0f;
  const float y1 = 1.0f - ((py + ph) / (float)screen_height) * 2.0f;

  const float quad[12] = {
      x0, y0, x1, y0, x0, y1,
      x0, y1, x1, y0, x1, y1};
  memcpy(out, quad, sizeof(quad));
  return out + 12;
}

static float *push_text(float *out, const char *text, float x, float y)
{
  float cursor_x = x;
  for (const char *c = text; *c && (c - text) < OVERLAY_MAX_CHARS; ++c)
  {
    const Glyph *g = glyph_lookup(*c);
    for (int row = 0; row < GLYPH_ROWS; ++row)
    {
      unsigned char bits = g->rows[row];
      for (int col = 0; col < GLYPH_COLS; ++col)
      {
        if (bits & (1 << (GLYPH_COLS - 1 - col)))
        {
          out = push_pixel_quad(out,
                                 cursor_x + col * GLYPH_SCALE,
                                 y + row * GLYPH_SCALE,
                                 GLYPH_SCALE, GLYPH_SCALE);
        }
      }
    }
    cursor_x += (GLYPH_COLS + GLYPH_SPACING) * GLYPH_SCALE;
  }
  return out;
}

void overlay_render(void)
{
  if (diag_get_egl_swap_count() < OVERLAY_WARMUP_SWAPS)
    return;

  if (!s_gl_ready)
  {
    s_gl_ready = overlay_gl_init() ? 1 : -1;
  }
  if (s_gl_ready <= 0)
    return;

  // --- FPS accounting -------------------------------------------------
  static u64 s_last_tick = 0;
  static int s_frames_since_update = 0;
  static int s_display_fps = 0;

  u64 now = armGetSystemTick();
  if (s_last_tick == 0)
    s_last_tick = now;
  s_frames_since_update++;

  double elapsed_s = armTicksToNs(now - s_last_tick) / 1e9;
  if (elapsed_s >= 0.5)
  {
    s_display_fps = (int)(s_frames_since_update / elapsed_s + 0.5);
    s_frames_since_update = 0;
    s_last_tick = now;
  }

  char text[OVERLAY_MAX_CHARS + 1];
  snprintf(text, sizeof(text), "%d FPS  %dx%d", s_display_fps, screen_width, screen_height);

  const float text_w = (float)strlen(text) * (GLYPH_COLS + GLYPH_SPACING) * GLYPH_SCALE;
  const float text_h = (float)GLYPH_ROWS * GLYPH_SCALE;
  const float panel_x = OVERLAY_MARGIN;
  const float panel_y = OVERLAY_MARGIN;
  const float panel_w = text_w + OVERLAY_PAD * 2;
  const float panel_h = text_h + OVERLAY_PAD * 2;

  // --- Save every bit of GL state we're about to touch -----------------
  GLint prev_program = 0, prev_array_buffer = 0;
  GLint prev_viewport[4] = {0, 0, 0, 0};
  GLint prev_blend_src_rgb = 0, prev_blend_dst_rgb = 0;
  GLint prev_blend_src_a = 0, prev_blend_dst_a = 0;
  GLint prev_attrib0_enabled = 0;
  glGetIntegerv(GL_CURRENT_PROGRAM, &prev_program);
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_array_buffer);
  glGetIntegerv(GL_VIEWPORT, prev_viewport);
  glGetIntegerv(GL_BLEND_SRC_RGB, &prev_blend_src_rgb);
  glGetIntegerv(GL_BLEND_DST_RGB, &prev_blend_dst_rgb);
  glGetIntegerv(GL_BLEND_SRC_ALPHA, &prev_blend_src_a);
  glGetIntegerv(GL_BLEND_DST_ALPHA, &prev_blend_dst_a);
  glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &prev_attrib0_enabled);
  GLboolean prev_depth_test = glIsEnabled(GL_DEPTH_TEST);
  GLboolean prev_blend = glIsEnabled(GL_BLEND);
  GLboolean prev_cull_face = glIsEnabled(GL_CULL_FACE);
  GLboolean prev_scissor = glIsEnabled(GL_SCISSOR_TEST);

  // --- Build vertex data ------------------------------------------------
  float *cursor = s_verts;
  cursor = push_pixel_quad(cursor, panel_x, panel_y, panel_w, panel_h);
  const int panel_verts = 6;
  cursor = push_text(cursor, text, panel_x + OVERLAY_PAD, panel_y + OVERLAY_PAD);
  const int total_verts = (int)((cursor - s_verts) / 2);

  // --- Draw --------------------------------------------------------------
  glViewport(0, 0, screen_width, screen_height);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_SCISSOR_TEST);
  glEnable(GL_BLEND);
  glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

  glUseProgram(s_program);
  glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
  glBufferData(GL_ARRAY_BUFFER, total_verts * 2 * sizeof(float), s_verts, GL_DYNAMIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void *)0);

  glUniform4f(s_uColor, 0.0f, 0.0f, 0.0f, 0.45f);
  glDrawArrays(GL_TRIANGLES, 0, panel_verts);

  glUniform4f(s_uColor, 1.0f, 1.0f, 1.0f, 1.0f);
  glDrawArrays(GL_TRIANGLES, panel_verts, total_verts - panel_verts);

  // --- Restore state exactly as the game left it ------------------------
  if (prev_attrib0_enabled)
    glEnableVertexAttribArray(0);
  else
    glDisableVertexAttribArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, prev_array_buffer);
  glUseProgram(prev_program);
  glBlendFuncSeparate(prev_blend_src_rgb, prev_blend_dst_rgb, prev_blend_src_a, prev_blend_dst_a);
  if (prev_blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
  if (prev_depth_test) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
  if (prev_cull_face) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
  if (prev_scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
  glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
}
