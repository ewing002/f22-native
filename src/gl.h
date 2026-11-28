// Minimal OpenGL 3.3 loader (no external library): functions above 1.1 are
// loaded via wglGetProcAddress.
#pragma once
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/wglext.h>

#define F22_GL_FUNCS(X)                                                    \
  X(PFNGLCREATESHADERPROC, glCreateShader)                                 \
  X(PFNGLSHADERSOURCEPROC, glShaderSource)                                 \
  X(PFNGLCOMPILESHADERPROC, glCompileShader)                               \
  X(PFNGLGETSHADERIVPROC, glGetShaderiv)                                   \
  X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog)                         \
  X(PFNGLCREATEPROGRAMPROC, glCreateProgram)                               \
  X(PFNGLATTACHSHADERPROC, glAttachShader)                                 \
  X(PFNGLLINKPROGRAMPROC, glLinkProgram)                                   \
  X(PFNGLGETPROGRAMIVPROC, glGetProgramiv)                                 \
  X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog)                       \
  X(PFNGLUSEPROGRAMPROC, glUseProgram)                                     \
  X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation)                     \
  X(PFNGLUNIFORM1IPROC, glUniform1i)                                       \
  X(PFNGLUNIFORM1FPROC, glUniform1f)                                       \
  X(PFNGLUNIFORM3FPROC, glUniform3f)                                       \
  X(PFNGLUNIFORM3FVPROC, glUniform3fv)                                     \
  X(PFNGLUNIFORMMATRIX3FVPROC, glUniformMatrix3fv)                         \
  X(PFNGLUNIFORMMATRIX4FVPROC, glUniformMatrix4fv)                         \
  X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays)                           \
  X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray)                           \
  X(PFNGLGENBUFFERSPROC, glGenBuffers)                                     \
  X(PFNGLBINDBUFFERPROC, glBindBuffer)                                     \
  X(PFNGLBUFFERDATAPROC, glBufferData)                                     \
  X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray)           \
  X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer)                   \
  X(PFNGLTEXIMAGE3DPROC, glTexImage3D)                                     \
  X(PFNGLGENERATEMIPMAPPROC, glGenerateMipmap)                             \
  X(PFNGLACTIVETEXTUREPROC, glActiveTexture)

#define F22_DECL(t, n) extern t n;
F22_GL_FUNCS(F22_DECL)
#undef F22_DECL

bool loadGL();
GLuint makeProgram(const char* vs, const char* fs);
