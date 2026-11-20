#include "gl.h"
#include <cstdio>
#include <string>

#define F22_DEF(t, n) t n = nullptr;
F22_GL_FUNCS(F22_DEF)
#undef F22_DEF

bool loadGL() {
  bool ok = true;
#define F22_LOAD(t, n)                                      \
  n = reinterpret_cast<t>(wglGetProcAddress(#n));            \
  if (!n) { std::fprintf(stderr, "Missing GL function: %s\n", #n); ok = false; }
  F22_GL_FUNCS(F22_LOAD)
#undef F22_LOAD
  return ok;
}

static GLuint compile(GLenum type, const char* src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, nullptr);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[4096];
    glGetShaderInfoLog(s, sizeof log, nullptr, log);
    MessageBoxA(nullptr, log, "Shader hiba", MB_ICONERROR);
    ExitProcess(1);
  }
  return s;
}

GLuint makeProgram(const char* vs, const char* fs) {
  GLuint p = glCreateProgram();
  glAttachShader(p, compile(GL_VERTEX_SHADER, vs));
  glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs));
  glLinkProgram(p);
  GLint ok = 0;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[4096];
    glGetProgramInfoLog(p, sizeof log, nullptr, log);
    MessageBoxA(nullptr, log, "Shader link error", MB_ICONERROR);
    ExitProcess(1);
  }
  return p;
}
