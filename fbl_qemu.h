//
// fbl_tinygles.h
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef _fbl_tinygles_h
#define _fbl_tinygles_h

#include <stdint.h>
#include "circle/interrupt.h"
#include "vc4/interface/vcinclude/vc_image_types.h"
#include "littlevgl/littlevgl.h"

// A wrapper that manages a single dispmanx layer and
// indexed frame buffer.
class FrameBufferLayer {
public:
  FrameBufferLayer();

  virtual ~FrameBufferLayer();

  // Sets the layer for this frame buffer. Must be called before Allocate.
  void SetLayer(int layer);

  int GetLayer(void);

  // Tells this fb to use a palette with transparency.
  // Must be called before Allocate.  If transparency is used, palette
  // calls must use the 32 bit ARGB format.  Otherwise, the RGB565 format
  // is used.
  void SetTransparency(bool transparency);

  // pixel mode: 0=8-bit-indexed, 1=RGB565
  // pitch represents bytes per line
  int Allocate(int pixelmode, uint8_t **pixels, int width, int height, int *pitch);

  // Allocate again with the same parameters. pixels and
  // pitch will remain the same.  Used for turning the shader
  // on/off.
  int ReAllocate(bool shader_enable);

  void Free();

  void Clear();

  // Get a pointer to raw pixel data for this frame buffer
  void* GetPixels();

  // Indicates raw pixel data has a complete frame. Upload to the
  // offscreen resource unless to_offscreen is 0, in which case
  // the currently visible resource is the destination.
  void FrameReady(int to_offscreen);

  // Show the framebuffer.
  void Show();

  // Hide the framebuffer.
  void Hide();

  // Set one color of the indexed palette. Can only be called when
  // transparency is false.
  void SetPalette(uint8_t index, uint16_t rgb565);

  // Set one color of the indexed palette. Can only be called when
  // transparency is true.
  void SetPalette(uint8_t index, uint32_t rgba);

  // Commit current palette to frame buffer
  void UpdatePalette();

  // Sets the region within the frame buffer to scale. Takes effect
  // on the next call to Show()
  void SetSrcRect(int x, int y, int w, int h);

  // The amount to stretch to vertical and horizontal dimensions
  // If hstretch is positive, the src region height is scaled up to the
  // height of the frame buffer * vstretch, the width is then determined by
  // frame buffer height * hstretch.
  // Otherwise, the src region width is scaled up to the width of the
  // frame buffer * vstretch, the height is determined by 
  // frame buffer width / hstretch.
  void SetStretch(double hstretch, double vstretch, int hintstr, int vintstr, int use_hintstr, int use_vintstr);

  void SetCenterOffset(int cx, int cy);

  // alignment can be -1 = ALIGN TOP, 0 = CENTER, 1 = ALIGN BOTTOM
  // padding applies to TOP or BOTTOM only.
  void SetVerticalAlignment(int alignment, int padding);

  // alignment can be -1 = ALIGN LEFT, 0 = CENTER, 1 = ALIGN RIGHT
  // padding applies to LEFT or RIGHT only.
  void SetHorizontalAlignment(int alignment, int padding);

  // Takes some space away from the full screen when determining destination rect
  // Used to force a fb into a smaller space (for things like PIP or side-by-side.
  void SetPadding(double leftPadding, double rightPadding, double topPadding, double bottomPadding);

  // Retrieve dimensions for this layer. 
  void GetDimensions(
    int *display_w, int *display_h,
    int *fb_w,      int *fb_h,
    int *src_w,     int *src_h,
    int *dst_w,     int *dst_h);

  // initializes the framebuffer
  static void Initialize();

  bool UsesShader();

  bool Showing();

  void SetUsesShader(bool enable);

  // NOTE: This will implicitly Hide the layer since the shader must be
  // destroyed and recompiled.
  void SetShaderParams(
    bool curvature,
    float curvature_x,
    float curvature_y,
    int mask,
    float mask_brightness,
    bool gamma,
    bool fake_gamma,
    bool scanlines,
    bool multisample,
    float scanline_weight,
    float scanline_gap_brightness,
    float bloom_factor,
    float input_gamma,
    float output_gamma,
    bool sharper,
    bool bilinear_interpolation);

  static void SetInterpolation(int enable);

  static void SwapResources(bool sync, FrameBufferLayer* fb1, FrameBufferLayer* fb2);

private:
  static bool initialized_;

  // Raw pixel data. Not VC memory.
  uint8_t* pixels_;

  int fb_width_;
  int fb_height_;
  int fb_pitch_;

  int layer_;
  int transparency_;

  double hstretch_;
  double vstretch_;
  int hintstr_;
  int vintstr_;
  int use_hintstr_;
  int use_vintstr_;

  // -1 = top, 0 = center, 1 = bottom
  int valign_;
  int vpadding_;
  // -1 = left, 0 = center, 1 = right
  int halign_;
  int hpadding_;

  int h_center_offset_;
  int v_center_offset_;

  // Used to take away some available display area
  // before deciding what the dest rect coords should be. Expressed
  // as percentage of available area.
  double leftPadding_;
  double rightPadding_;
  double topPadding_;
  double bottomPadding_;

  int display_width_;
  int display_height_;

  int src_x_;
  int src_y_;
  int src_w_;
  int src_h_;

  int dst_x_;
  int dst_y_;
  int dst_w_;
  int dst_h_;

  bool showing_;
  bool allocated_;

  int bytes_per_pixel_;

  uint16_t pal_565_[256];
  uint32_t pal_argb_[256];
};

#endif
