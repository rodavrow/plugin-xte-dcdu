// XTE-DCDU: cross-platform OpenGL function pointer loader for the bits we
// actually use (FBO + PBO). X-Plane 12 ships with a GL >= 3.0 context on every
// platform, so these symbols are guaranteed to resolve at runtime.
// Copyright (C) 2026 David Rowlandson
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <GL/gl.h>
#elif defined(__APPLE__)
  #include <OpenGL/gl.h>
  #include <OpenGL/glext.h>
#else
  #define GL_GLEXT_PROTOTYPES 1
  #include <GL/gl.h>
  #include <GL/glext.h>
#endif

// Make sure the tokens exist on platforms whose <GL/gl.h> is GL 1.1.
#ifndef GL_PIXEL_PACK_BUFFER
#define GL_PIXEL_PACK_BUFFER          0x88EB
#endif
#ifndef GL_PIXEL_PACK_BUFFER_BINDING
#define GL_PIXEL_PACK_BUFFER_BINDING  0x88ED
#endif
#ifndef GL_STREAM_READ
#define GL_STREAM_READ                0x88E1
#endif
#ifndef GL_MAP_READ_BIT
#define GL_MAP_READ_BIT               0x0001
#endif
#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER           0x8CA8
#endif
#ifndef GL_DRAW_FRAMEBUFFER
#define GL_DRAW_FRAMEBUFFER           0x8CA9
#endif
#ifndef GL_DRAW_FRAMEBUFFER_BINDING
#define GL_DRAW_FRAMEBUFFER_BINDING   0x8CA6
#endif
#ifndef GL_READ_FRAMEBUFFER_BINDING
#define GL_READ_FRAMEBUFFER_BINDING   0x8CAA
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0          0x8CE0
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE       0x8CD5
#endif

namespace xtedcdu::gl {

// FBO
typedef void (APIENTRY *PFN_glGenFramebuffers)(GLsizei, GLuint*);
typedef void (APIENTRY *PFN_glDeleteFramebuffers)(GLsizei, const GLuint*);
typedef void (APIENTRY *PFN_glBindFramebuffer)(GLenum, GLuint);
typedef void (APIENTRY *PFN_glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);

typedef GLenum (APIENTRY *PFN_glCheckFramebufferStatus)(GLenum);

// PBO / buffer object
typedef void   (APIENTRY *PFN_glGenBuffers)(GLsizei, GLuint*);
typedef void   (APIENTRY *PFN_glDeleteBuffers)(GLsizei, const GLuint*);
typedef void   (APIENTRY *PFN_glBindBuffer)(GLenum, GLuint);
typedef void   (APIENTRY *PFN_glBufferData)(GLenum, ptrdiff_t, const void*, GLenum);
typedef void*  (APIENTRY *PFN_glMapBufferRange)(GLenum, ptrdiff_t, ptrdiff_t, GLbitfield);
typedef GLboolean (APIENTRY *PFN_glUnmapBuffer)(GLenum);

extern PFN_glGenFramebuffers           p_glGenFramebuffers;
extern PFN_glDeleteFramebuffers        p_glDeleteFramebuffers;
extern PFN_glBindFramebuffer           p_glBindFramebuffer;
extern PFN_glFramebufferTexture2D      p_glFramebufferTexture2D;
extern PFN_glCheckFramebufferStatus    p_glCheckFramebufferStatus;
extern PFN_glGenBuffers            p_glGenBuffers;
extern PFN_glDeleteBuffers         p_glDeleteBuffers;
extern PFN_glBindBuffer            p_glBindBuffer;
extern PFN_glBufferData            p_glBufferData;
extern PFN_glMapBufferRange        p_glMapBufferRange;
extern PFN_glUnmapBuffer           p_glUnmapBuffer;

// Resolve all function pointers. Returns true on full success. Safe to call
// multiple times. Must be called with a current GL context (i.e. from inside
// a draw callback).
bool load();

} // namespace xtedcdu::gl
