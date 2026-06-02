// Copyright (C) 2026 David Rowlandson
// SPDX-License-Identifier: GPL-3.0-or-later
#include "gl_loader.hpp"
#include "log.hpp"

#if defined(_WIN32)
  #include <windows.h>
#elif defined(__APPLE__)
  #include <dlfcn.h>
#else
  #include <dlfcn.h>
#endif

namespace xtedcdu::gl {

PFN_glGenFramebuffers           p_glGenFramebuffers           = nullptr;
PFN_glDeleteFramebuffers        p_glDeleteFramebuffers        = nullptr;
PFN_glBindFramebuffer           p_glBindFramebuffer           = nullptr;
PFN_glFramebufferTexture2D      p_glFramebufferTexture2D      = nullptr;
PFN_glCheckFramebufferStatus    p_glCheckFramebufferStatus    = nullptr;
PFN_glGenBuffers            p_glGenBuffers            = nullptr;
PFN_glDeleteBuffers         p_glDeleteBuffers         = nullptr;
PFN_glBindBuffer            p_glBindBuffer            = nullptr;
PFN_glBufferData            p_glBufferData            = nullptr;
PFN_glMapBufferRange        p_glMapBufferRange        = nullptr;
PFN_glUnmapBuffer           p_glUnmapBuffer           = nullptr;

namespace {

void* resolve(const char* name) {
#if defined(_WIN32)
    void* p = (void*)wglGetProcAddress(name);
    if (p == nullptr || p == (void*)0x1 || p == (void*)0x2 ||
        p == (void*)0x3 || p == (void*)-1) {
        HMODULE m = GetModuleHandleA("opengl32.dll");
        if (m) p = (void*)GetProcAddress(m, name);
        else   p = nullptr;
    }
    return p;
#elif defined(__APPLE__)
    static void* gl = dlopen(
        "/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_LAZY);
    return gl ? dlsym(gl, name) : nullptr;
#else
    static void* gl = dlopen("libGL.so.1", RTLD_LAZY | RTLD_GLOBAL);
    if (!gl) gl  = dlopen("libGL.so",   RTLD_LAZY | RTLD_GLOBAL);
    return gl ? dlsym(gl, name) : nullptr;
#endif
}

template <typename T>
bool load_one(T& slot, const char* name) {
    if (slot) return true;
    slot = reinterpret_cast<T>(resolve(name));
    if (!slot) {
        XTED_ERR("gl_loader: failed to resolve %s", name);
        return false;
    }
    return true;
}

} // namespace

bool load() {
    bool ok = true;
    ok &= load_one(p_glGenFramebuffers,           "glGenFramebuffers");
    ok &= load_one(p_glDeleteFramebuffers,        "glDeleteFramebuffers");
    ok &= load_one(p_glBindFramebuffer,           "glBindFramebuffer");
    ok &= load_one(p_glFramebufferTexture2D,      "glFramebufferTexture2D");
    ok &= load_one(p_glCheckFramebufferStatus,    "glCheckFramebufferStatus");
    ok &= load_one(p_glGenBuffers,           "glGenBuffers");
    ok &= load_one(p_glDeleteBuffers,        "glDeleteBuffers");
    ok &= load_one(p_glBindBuffer,           "glBindBuffer");
    ok &= load_one(p_glBufferData,           "glBufferData");
    ok &= load_one(p_glMapBufferRange,       "glMapBufferRange");
    ok &= load_one(p_glUnmapBuffer,          "glUnmapBuffer");
    return ok;
}

} // namespace xtedcdu::gl
