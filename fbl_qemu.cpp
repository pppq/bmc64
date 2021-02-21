//
// fbl_qemu.cpp
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

#include "fbl_qemu.h"

#include <set>
#include <limits.h>
#include <malloc.h>
#include <string.h>
#include <stdio.h>
#include "circle/bcmframebuffer.h"
#include "circle/timer.h"

#ifndef ALIGN_UP
#define ALIGN_UP(x,y)  ((x + (y)-1) & ~((y)-1))
#endif

#define RGB565(r,g,b) (((r)>>3)<<11 | ((g)>>2)<<5 | (b)>>3)
#define ARGB(a,r,g,b) ((uint32_t)((uint8_t)(a)<<24 | (uint8_t)(r)<<16 | (uint8_t)(g)<<8 | (uint8_t)(b)))

// Default palette used for canvases with no transparency
static uint16_t pal_565[256] = {
  RGB565(0x00, 0x00, 0x00),
  RGB565(0xFF, 0xFF, 0xFF),
  RGB565(0xFF, 0x00, 0x00),
  RGB565(0x70, 0xa4, 0xb2),
  RGB565(0x6f, 0x3d, 0x86),
  RGB565(0x58, 0x8d, 0x43),
  RGB565(0x35, 0x28, 0x79),
  RGB565(0xb8, 0xc7, 0x6f),
  RGB565(0x6f, 0x4f, 0x25),
  RGB565(0x43, 0x39, 0x00),
  RGB565(0x9a, 0x67, 0x59),
  RGB565(0x44, 0x44, 0x44),
  RGB565(0x6c, 0x6c, 0x6c),
  RGB565(0x9a, 0xd2, 0x84),
  RGB565(0x6c, 0x5e, 0xb5),
  RGB565(0x95, 0x95, 0x95),
};

// Default palette used for canvases with transparency. This palette
// is identical to pal_565 except we include an additional
// entry for a fully transparent pixel (index 16).
static uint32_t pal_argb[256] = {
  // First 16 colors are opaque
  ARGB(0xFF, 0x00, 0x00, 0x00),
  ARGB(0xFF, 0xFF, 0xFF, 0xFF),
  ARGB(0xFF, 0xFF, 0x00, 0x00),
  ARGB(0xFF, 0x70, 0xa4, 0xb2),
  ARGB(0xFF, 0x6f, 0x3d, 0x86),
  ARGB(0xFF, 0x58, 0x8d, 0x43),
  ARGB(0xFF, 0x35, 0x28, 0x79),
  ARGB(0xFF, 0xb8, 0xc7, 0x6f),
  ARGB(0xFF, 0x6f, 0x4f, 0x25),
  ARGB(0xFF, 0x43, 0x39, 0x00),
  ARGB(0xFF, 0x9a, 0x67, 0x59),
  ARGB(0xFF, 0x44, 0x44, 0x44),
  ARGB(0xFF, 0x6c, 0x6c, 0x6c),
  ARGB(0xFF, 0x9a, 0xd2, 0x84),
  ARGB(0xFF, 0x6c, 0x5e, 0xb5),
  ARGB(0xFF, 0x95, 0x95, 0x95),

  // Use index 16 for fully transparent pixels
  ARGB(0x00, 0x00, 0x00, 0x00),
};

#define SCREEN_W 768
#define SCREEN_H 544

static CBcmFrameBuffer *bcmFrameBuffer;
static uint32_t frameBufferOffsetY = 0;
static uint32_t frameBufferPageSize;
static uint32_t frameBufferPitch;
static uint16_t *frameBuffer;

bool FrameBufferLayer::initialized_ = false;

static bool cmp_layer(FrameBufferLayer *a, FrameBufferLayer *b) {
    return a->GetLayer() < b->GetLayer();
}

static std::set<FrameBufferLayer*, decltype(cmp_layer)*> visibleLayers(cmp_layer);

// static
void FrameBufferLayer::Initialize() {
  if (initialized_) {
    return;
  }

  /* 
   * Allocate framebuffer (need at least 1 pixel wider viewport for virtual offset to work)
   * https://github.com/qemu/qemu/blob/d6798cc01d6edabaa4e326359b69f08d022bf4c7/hw/display/bcm2835_fb.c#L140-L150
   */
  bcmFrameBuffer = new CBcmFrameBuffer(SCREEN_W, SCREEN_H, 16, SCREEN_W + 1, (SCREEN_H << 1));
  bcmFrameBuffer->Initialize();

  frameBufferPitch = bcmFrameBuffer->GetPitch();
  frameBufferPageSize = frameBufferPitch * SCREEN_H;
  frameBuffer = (uint16_t *) (uintptr) bcmFrameBuffer->GetBuffer();

  initialized_ = true;
}

FrameBufferLayer::FrameBufferLayer() :
  pixels_(nullptr),
  fb_width_(0), fb_height_(0), fb_pitch_(0), 
  layer_(0), 
  transparency_(false),
  hstretch_(1.6), vstretch_(1.0), 
  hintstr_(0), vintstr_(0),
  use_hintstr_(0), use_vintstr_(0),
  valign_(0), vpadding_(0), 
  halign_(0), hpadding_(0),
  h_center_offset_(0), v_center_offset_(0),
  leftPadding_(0), rightPadding_(0), 
  topPadding_(0), bottomPadding_(0),
  display_width_(0), display_height_(0),
  src_x_(0), src_y_(0), src_w_(0), src_h_(0),
  dst_x_(0), dst_y_(0), dst_w_(0), dst_h_(0),
  showing_(false), 
  allocated_(false),
  bytes_per_pixel_(1) {

  memcpy (pal_565_, pal_565, sizeof(pal_565));
  memcpy (pal_argb_, pal_argb, sizeof(pal_argb));
}

FrameBufferLayer::~FrameBufferLayer() {
  if (showing_) { Hide(); }
  if (allocated_) { Free(); }
}

void FrameBufferLayer::SetUsesShader(bool enabled) {
  // Nothing to do here
}

void FrameBufferLayer::SetShaderParams(
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
    bool bilinear_interpolation) {

  // Not using shaders, but still need to hide the layer
  Hide();
}

int FrameBufferLayer::Allocate(
  int pixelmode, 
  uint8_t **pixels,
  int width, 
  int height, 
  int *pitch) {

  assert(!allocated_);
  allocated_ = true;

  switch (pixelmode) {
     case 0:
        bytes_per_pixel_ = 1;
        break;
     case 1:
        bytes_per_pixel_ = 2;
        break;
     default:
        bytes_per_pixel_ = 1;
        break;
  }

  // pitch is in bytes
  if (pitch) {
     *pitch = fb_pitch_ = ALIGN_UP(width * bytes_per_pixel_, 32);
  }

  fb_width_ = width;
  fb_height_ = height;

  display_width_ = SCREEN_W;
  display_height_ = SCREEN_H;

  if (pixels) {
     pixels_ = (uint8_t*) malloc(fb_pitch_ * height);
     *pixels = pixels_;
  }

  if (pixels) {
     // Don't clobber these on realloc.
     dst_x_ = 0;
     dst_y_ = 0;
     dst_w_ = width;
     dst_h_ = height;

     src_x_ = 0;
     src_y_ = 0;
     src_w_ = width;
     src_h_ = height;
  }

  resources_[0] = (uint16_t *) malloc(width * height * 2);
  resources_[1] = (uint16_t *) malloc(width * height * 2);

  return 0;
}

int FrameBufferLayer::ReAllocate(bool shader_enable) {
  // We will act like shader has been enabled or disabled, but not actually do anything
  assert(allocated_);
  return 0;
}

void FrameBufferLayer::Clear() {
  assert (allocated_);
  memset(pixels_, 0, fb_height_ * fb_pitch_);
}

void FrameBufferLayer::Free() {
  if (!allocated_) return;
  if (showing_) { Hide(); }

  fb_width_ = 0;
  fb_height_ = 0;
  fb_pitch_ = 0;
  free(pixels_);

  free(resources_[0]);
  free(resources_[1]);

  allocated_ = false;
}

void FrameBufferLayer::Show() {
  if (showing_) return;

  int dst_w;
  int dst_h;
  assert (hstretch_ != 0);
  assert (vstretch_ != 0);

  int lpad_abs = display_width_ * leftPadding_;
  int rpad_abs = display_width_ * rightPadding_;
  int tpad_abs = display_height_ * topPadding_;
  int bpad_abs = display_height_ * bottomPadding_;

  int avail_width = display_width_ - lpad_abs - rpad_abs;
  int avail_height = display_height_ - tpad_abs - bpad_abs;

  if (hstretch_ < 0) {
     // Stretch horizontally to fill width * vstretch and then set height
     // based on hstretch.  This mode doesn't support integer stretch.
     dst_w = avail_width * vstretch_;
     dst_h = avail_width / -hstretch_;
     if (dst_w > avail_width) {
        dst_w = avail_width;
     }
     if (dst_h > avail_height) {
        dst_h = avail_height;
     }
  } else {
     // Stretch vertically to fill height * vstretch and then set width
     // based on hstretch.
     dst_h = avail_height * vstretch_;
     if (use_vintstr_) {
        dst_h = vintstr_;
     }
     dst_w = avail_height * hstretch_;
     if (use_hintstr_) {
        dst_w = hintstr_;
     }

     if (dst_h > avail_height) {
        dst_h = avail_height;
     }
     if (dst_w > avail_width) {
        dst_w = avail_width;
     }
  }

  // Resulting image is centered
  int oy;
  switch (valign_) {
     case 0:
        // Center
        oy = (avail_height - dst_h) / 2 + v_center_offset_;
        break;
     case -1:
        // Top
        oy = vpadding_;
        break;
     case 1:
        // Bottom
        oy = avail_height - dst_h - vpadding_;
        break;
     default:
        oy = 0;
        break;
  }

  int ox;
  switch (halign_) {
     case 0:
        // Center
        ox = (avail_width - dst_w) / 2 + h_center_offset_;
        break;
     case -1:
        // Left
        ox = hpadding_;
        break;
     case 1:
        // Right
        ox = avail_width - dst_w - hpadding_;
        break;
     default:
        ox = 0;
        break;
  }


  dst_x_ = ox + lpad_abs;
  dst_y_ = oy + tpad_abs;
  dst_w_ = dst_w;
  dst_h_ = dst_h;

  visibleLayers.insert(this);
  FrameReady(0);
  showing_ = true;
  SwapResources(false, this, nullptr);
}

void FrameBufferLayer::Hide() {
  if (!showing_) return;
  showing_ = false;
  visibleLayers.erase(this);
}

void* FrameBufferLayer::GetPixels() {
  return pixels_;
}

void FrameBufferLayer::FrameReady(int to_offscreen) {
  // Copy data into either the offscreen resource (if to_offscreen) or the
  // on screen resource (if !to_offscreen). Only color conversion is happening here.
  int rnum = to_offscreen ? 1 - rnum_ : rnum_;
  uint16_t *dst = resources_[rnum];

  if (bytes_per_pixel_ == 1) {
    uint8_t *src = pixels_;

    for (int y = 0; y < fb_height_; y++) {
      for (int x = 0; x < fb_width_; x++) {
        uint8_t srcIdx = src[x];
        
        uint8_t a;
        uint8_t r;
        uint8_t g;
        uint8_t b;

        if (transparency_) {
          uint32_t srcCol32 = pal_argb_[srcIdx];
          
          a = (srcCol32 >> 24) > 0 ? 1 : 0;
          r = (srcCol32 >> 19) & 31;
          g = (srcCol32 >> 11) & 31;
          b = (srcCol32 >> 3)  & 31;
        } else {
          uint16_t srcCol16 = pal_565_[srcIdx];
          
          a = 1;
          r = (srcCol16 >> 11) & 31;
          g = (srcCol16 >> 6)  & 31;
          b = srcCol16         & 31;
        }

        dst[x] = (a << 15) | (r << 10) | (g << 5) | b;
      }
      
      src += fb_pitch_;
      dst += fb_width_;
    }
  } else {
    uint16_t *src = (uint16_t *) pixels_;

    for (int y = 0; y < fb_height_; y++) {
     for (int x = 0; x < fb_width_; x++) {
        uint16_t srcCol16 = src[x];
          
        uint8_t a = 1;
        uint8_t r = (srcCol16 >> 11) & 31;
        uint8_t g = (srcCol16 >> 6)  & 31;
        uint8_t b = srcCol16         & 31;

        dst[x] = (a << 15) | (r << 10) | (g << 5) | b;
      }

      src += fb_pitch_;
      dst += fb_width_;
    }
  }
}

#define FPS 50
static unsigned lastTicks = 0;

// static
void FrameBufferLayer::SwapResources(
  bool sync,
  FrameBufferLayer* fb1,
  FrameBufferLayer* fb2) {

  if (fb1 && sync) { fb1->rnum_ = 1 - fb1->rnum_; }
  if (fb2 && sync) { fb2->rnum_ = 1 - fb2->rnum_; }

  frameBufferOffsetY = SCREEN_H - frameBufferOffsetY;

  // Erase content around the first layer
  boolean first = true;

  // Re-draw all visible layers in order
  std::set<FrameBufferLayer*, decltype(cmp_layer)*>::iterator it;
  for (it = visibleLayers.begin(); it != visibleLayers.end(); ++it) {
    FrameBufferLayer *current = *it;

    uint16_t *src = current->resources_[current->rnum_]
      + (current->src_y_ * current->fb_width_) 
      + current->src_x_;

    uint16_t *dst;
    
    if (first) {
      dst = (uint16_t *) (((uintptr) frameBuffer)
        + (frameBufferOffsetY * frameBufferPitch));
    } else {
      dst = (uint16_t *) (((uintptr) frameBuffer)
        + (frameBufferOffsetY * frameBufferPitch)
        + (current->dst_y_ * frameBufferPitch)
        + (current->dst_x_ << 1));
    }

    int budget_y = 0;

    // erase top edge
    if (first && current->dst_y_ > 0) {
      memset(dst, 0x00, current->dst_y_ * frameBufferPitch);
      dst += ((current->dst_y_ * frameBufferPitch) >> 1);
    }

    for (int dy = 0; dy < current->dst_h_; dy++) {
      budget_y += current->src_h_;
      while (budget_y > current->dst_h_) {
        budget_y -= current->dst_h_;
        src += current->fb_width_;
      }
      
      // erase left edge
      if (first && current->dst_x_ > 0) {
        memset(dst, 0x00, (current->dst_x_ << 1));
        dst += current->dst_x_;
      }

      int budget_x = 0;
      int sx = 0;

      for (int dx = 0; dx < current->dst_w_; dx++) {
        budget_x += current->src_w_;
        while (budget_x > current->dst_w_) {
          budget_x -= current->dst_w_;
          sx++;
        }
        
        uint16_t srcCol = src[sx];
        uint8_t a = srcCol >> 15;

        // 1-bit alpha
        if (a != 0) {
          uint8_t sr = (srcCol >> 10) & 31;
          uint8_t sg = (srcCol >> 5) & 31;
          uint8_t sb = srcCol & 31;

          // 5-to-6 bit conversion: shift in MSB from the left
          sg = (sg << 1) | (sg & 24);

          dst[dx] = (sr << 11) | (sg << 5) | sb;
        }
      }
      
      // erase right edge
      if (first) {
        int32_t rightEdge = SCREEN_W - current->dst_w_ - current->dst_x_;
        if (rightEdge > 0) { memset(dst + current->dst_w_, 0x00, (rightEdge << 1)); }
        dst -= current->dst_x_; // dst_x_ can be 0, in which case this is a no-op
      }
      
      dst += (frameBufferPitch >> 1);
    }

    // erase bottom edge; the next layer is no longer the first one
    if (first) {
      int32_t bottomEdge = SCREEN_H - current->dst_h_ - current->dst_y_;
      if (bottomEdge > 0) { memset(dst, 0x00, bottomEdge * frameBufferPitch); }
      first = false;
    }
  }

  bcmFrameBuffer->SetVirtualOffset(0, frameBufferOffsetY);
  
  if (sync) {
    // QEMU does not support waiting for VSync yet, use timer-based FPS limiter
#if 0
    bcmFrameBuffer->WaitForVerticalSync();
#else
    unsigned ticks;
    unsigned delta = 0;

    while (delta < CLOCKHZ / FPS) {
      ticks = CTimer::GetClockTicks();
      if (ticks >= lastTicks) {
        delta = ticks - lastTicks;
      } else {
        delta = UINT32_MAX - lastTicks + 1 + ticks;
      }
    }

    lastTicks = ticks;
#endif
  }
}

void FrameBufferLayer::SetPalette(uint8_t index, uint16_t rgb565) {
  assert(!transparency_);
  assert(bytes_per_pixel_ == 1);
  pal_565_[index] = rgb565;
}

void FrameBufferLayer::SetPalette(uint8_t index, uint32_t argb) {
  assert(transparency_);
  assert(bytes_per_pixel_ == 1);
  pal_argb_[index] = argb;
}

void FrameBufferLayer::UpdatePalette() {
  // Using RGB565 on the framebuffer, color conversion happens when frame is ready
}

void FrameBufferLayer::SetLayer(int layer) {
  layer_ = layer;
}

int FrameBufferLayer::GetLayer() {
  return layer_;
}

bool FrameBufferLayer::UsesShader() {
	return false;
}

bool FrameBufferLayer::Showing() {
	return showing_;
}

void FrameBufferLayer::SetTransparency(bool transparency) {
  transparency_ = transparency;
}

void FrameBufferLayer::SetSrcRect(int x, int y, int w, int h) {
  src_x_ = x;
  src_y_ = y;
  src_w_ = w;
  src_h_ = h;
}

// Set horizontal/vertical multipliers
void FrameBufferLayer::SetStretch(
  double hstretch, double vstretch, 
  int hintstr, int vintstr,
  int use_hintstr, int use_vintstr) {

  hstretch_ = hstretch;
  vstretch_ = vstretch;
  hintstr_ = hintstr;
  vintstr_ = vintstr;
  use_hintstr_ = use_hintstr;
  use_vintstr_ = use_vintstr;
}

void FrameBufferLayer::SetVerticalAlignment(int alignment, int padding) {
  valign_ = alignment;
  vpadding_ = padding;
}

void FrameBufferLayer::SetHorizontalAlignment(int alignment, int padding) {
  halign_ = alignment;
  hpadding_ = padding;
}

void FrameBufferLayer::SetPadding(
  double leftPadding,
  double rightPadding,
  double topPadding,
  double bottomPadding) {

  leftPadding_ = leftPadding;
  rightPadding_ = rightPadding;
  topPadding_ = topPadding;
  bottomPadding_ = bottomPadding;
}

void FrameBufferLayer::SetCenterOffset(int cx, int cy) {
  h_center_offset_ = cx;
  v_center_offset_ = cy;
}

void FrameBufferLayer::GetDimensions(
  int *display_w, int *display_h,
  int *fb_w, int *fb_h,
  int *src_w, int *src_h,
  int *dst_w, int *dst_h) {

  *display_w = display_width_;
  *display_h = display_height_;
  *fb_w = fb_width_;
  *fb_h = fb_height_;
  *src_w = src_w_;
  *src_h = src_h_;
  *dst_w = dst_w_;
  *dst_h = dst_h_;
}

// static
void FrameBufferLayer::SetInterpolation(int enable) {
  // Nothing to do here
}
