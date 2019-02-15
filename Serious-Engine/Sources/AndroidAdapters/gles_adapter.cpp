typedef double GLdouble;
typedef double GLclampd;

#include <GLES2/gl2.h>
#include <AndroidAdapters/gles_adapter.h>
#include <stdlib.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/ext.hpp>
#include <vector>

#define GLM_ENABLE_EXPERIMENTAL

#include <glm/gtx/string_cast.hpp>

void reportError(const char *func);

void blockingError(const char *func);

/**
 * TODO:
 *   glBegin, glColor and other direct mode
 *   glMatrixMode and other transformations
 *   modes:
 *     GL_CLIP_PLANE0
 *   glShadeModel
 *   glPolygonMode
 *   glDrawBuffer
 *   glEnableClientState
 */

#ifndef GL_VERTEX_ARRAY
#define GL_VERTEX_ARRAY 0x8074
#endif
#ifndef GL_TEXTURE_COORD_ARRAY
#define GL_TEXTURE_COORD_ARRAY 0x8078
#endif
#ifndef GL_DOUBLE
#define GL_DOUBLE 0x140A
#endif

#define GL_MATRIX_MODE 0x0BA0
#define GL_MODELVIEW 0x1700
#define GL_PROJECTION 0x1701
#define GL_TEXTURE 0x1702
#define GL_COLOR 0x1800
#define GL_QUADS 0x0007
#define GL_NORMAL_ARRAY 0x8075
#define GL_COLOR_ARRAY 0x8076
#define GL_MODULATE 0x2100
#define GL_TEXTURE_ENV 0x2300
#define GL_TEXTURE_ENV_MODE 0x2200
#define GL_CLAMP 0x2900
#define GL_ALPHA_TEST 0x0BC0

namespace gles_adapter {
  struct GenericBuffer {
      GLint size;
      GLenum type;
      GLsizei stride;
      const GLvoid *ptr;
  };

  bool USE_BUFFER_DATA = false;
  bool enableDraws = false;

  GLuint INDEX_POSITION = 1;
  GLuint INDEX_NORMAL = 2;
  GLuint INDEX_COLOR = 3;
  GLuint INDEX_TEXTURE_COORD = 4;

  // used in glDrawArrays to convert GL_QUADS into GL_TRIANGLES
  GLuint INDEX_DUMMY_ELEMENT_BUFFER = 10;
  std::vector<uint16_t> dummyElementBuffer;

  // Used to convert GL_UNSIGNED_INT into GL_UNSIGNED_SHORT in glDrawElements
  std::vector<uint16_t> dummyIndexBuffer;

  GLuint program;
  GLenum lastError = 0;

  GenericBuffer vp, tp, cp;

  const char *VERTEX_SHADER = R"***(
    precision highp float;

    attribute vec3 position;
    attribute vec3 normal;
    attribute vec4 color;
    attribute vec4 textureCoord;

    uniform mat4 projMat;
    uniform mat4 modelViewMat;

    varying vec4 vColor;
    varying vec2 vTexCoord;

    void main() {
      gl_Position = projMat * modelViewMat * vec4(position.xyz, 1.0);
      vColor = color;
      vTexCoord = textureCoord.xy;
    }

  )***";

  const char *FRAGMENT_SHADER = R"***(
    precision highp float;

    uniform sampler2D mainTexture;
    uniform float enableTexture;
    uniform float enableAlphaTest;

    varying vec4 vColor;
    varying vec2 vTexCoord;

    void main() {
      vec4 finalColor = vColor;
      if (enableTexture > 0.5) finalColor = finalColor * texture2D(mainTexture, vTexCoord);
      gl_FragColor = finalColor * vColor;

      // alpha test
      if (enableAlphaTest > 0.5 && gl_FragColor.w < 0.5) {
        discard;
      }
    }

  )***";

  bool isGL_ALPHA_TEST = false;
  bool isGL_TEXTURE_2D = false;
  bool isGL_VERTEX_ARRAY = false;
  bool isGL_TEXTURE_COORD_ARRAY = false;
  bool isGL_NORMAL_ARRAY = false;
  bool isGL_COLOR_ARRAY = false;

  // glGetTexEnv
  int val_GL_TEXTURE_ENV_MODE = GL_MODULATE;

  void installGlLogger();

  glm::mat4 modelViewMat = glm::mat4(1);
  glm::mat4 projMat = glm::mat4(1);
  glm::mat4 *currentMatrix = &modelViewMat;

  GLint projMatIdx, modelViewMatIdx, mainTextureLoc, enableTextureLoc, enableAlphaTestLoc;
  GLenum alphaTestFunc;
  GLclampf alphaTestRef;

  GLuint compileShader(GLenum type, const char *name, const char *source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    // check status
    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
      char buffer[2001] = "";
      glGetShaderInfoLog(shader, 2000, nullptr, buffer);
      static char error[2500];
      sprintf(error, "Cannot compile %s: %s", name, buffer);
      throw error;
    }

    return shader;
  }

  void gles_adp_init() {
    GLint success;

    // create program
    program = glCreateProgram();
    glAttachShader(program, compileShader(GL_VERTEX_SHADER, "vertex shader", VERTEX_SHADER));
    glAttachShader(program, compileShader(GL_FRAGMENT_SHADER, "fragment shader", FRAGMENT_SHADER));
    glBindAttribLocation(program, INDEX_POSITION, "position");
    glBindAttribLocation(program, INDEX_NORMAL, "normal");
    glBindAttribLocation(program, INDEX_COLOR, "color");
    glBindAttribLocation(program, INDEX_TEXTURE_COORD, "textureCoord");
    glLinkProgram(program);

    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
      throw "Cannot link program";
    }

    if (glGetError()) {
      throw "Cannot create program";
    }

    // check if client buffer is enabled on this system
    float dummy[10];
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexAttribPointer(INDEX_POSITION, 1, GL_FLOAT, false, 0, &dummy);
    glEnableVertexAttribArray(INDEX_COLOR);
    glDrawArrays(GL_TRIANGLES, 0, 0);
    if (glGetError()){
      USE_BUFFER_DATA = true;
    }

    glUseProgram(program);

    projMatIdx = glGetUniformLocation(program, "projMat");
    modelViewMatIdx = glGetUniformLocation(program, "modelViewMat");
    mainTextureLoc = glGetUniformLocation(program, "mainTexture");
    if (mainTextureLoc >= 0) glUniform1i(mainTextureLoc, 0);
    enableTextureLoc = glGetUniformLocation(program, "enableTexture");
    enableAlphaTestLoc = glGetUniformLocation(program, "enableAlphaTest");
  }

  void syncBuffers(GLsizei vertices) {
    uint32_t totalSize;
    // upload vertex buffer
    if (vp.type != GL_FLOAT) blockingError("unimplemented mode");
    if (USE_BUFFER_DATA) {
      totalSize = (vp.stride ? vp.stride : 3 * sizeof(float)) * vertices;
      glBindBuffer(GL_ARRAY_BUFFER, INDEX_POSITION);
      glBufferData(GL_ARRAY_BUFFER, totalSize, vp.ptr, GL_STREAM_DRAW);
      glVertexAttribPointer(INDEX_POSITION, vp.size, vp.type, GL_FALSE, vp.stride, 0);
      glBindBuffer(GL_ARRAY_BUFFER, 0);
    } else {
      glVertexAttribPointer(INDEX_POSITION, vp.size, vp.type, GL_FALSE, vp.stride, vp.ptr);
    }
    glEnableVertexAttribArray(INDEX_POSITION);

    // upload Texture buffer
    if (isGL_TEXTURE_COORD_ARRAY) {
      if (tp.type != GL_FLOAT) blockingError("unimplemented mode");
      if (USE_BUFFER_DATA) {
        totalSize = (tp.stride ? tp.stride : tp.size * sizeof(float)) * vertices;
        glBindBuffer(GL_ARRAY_BUFFER, INDEX_TEXTURE_COORD);
        glBufferData(GL_ARRAY_BUFFER, totalSize, tp.ptr, GL_STREAM_DRAW);
        glVertexAttribPointer(INDEX_TEXTURE_COORD, tp.size, tp.type, GL_FALSE, tp.stride, 0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
      } else {
        glVertexAttribPointer(INDEX_TEXTURE_COORD, tp.size, tp.type, GL_FALSE, tp.stride, tp.ptr);
      }
      glEnableVertexAttribArray(INDEX_TEXTURE_COORD);
    }

    // upload Color buffer
    if (isGL_COLOR_ARRAY) {
      if (cp.type != GL_UNSIGNED_BYTE) blockingError("unimplemented mode");
      if (USE_BUFFER_DATA) {
        totalSize = (cp.stride ? cp.stride : cp.size) * vertices;
        glBindBuffer(GL_ARRAY_BUFFER, INDEX_COLOR);
        glBufferData(GL_ARRAY_BUFFER, totalSize, cp.ptr, GL_STREAM_DRAW);
        glVertexAttribPointer(INDEX_COLOR, cp.size, cp.type, GL_TRUE, cp.stride, 0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
      } else {
        glVertexAttribPointer(INDEX_COLOR, cp.size, cp.type, GL_TRUE, cp.stride, cp.ptr);
      }
      glEnableVertexAttribArray(INDEX_COLOR);
    }

    // uniforms
    glUniformMatrix4fv(projMatIdx, 1, GL_FALSE, glm::value_ptr(projMat));
    glUniformMatrix4fv(modelViewMatIdx, 1, GL_FALSE, glm::value_ptr(modelViewMat));
    glUniform1f(enableTextureLoc, isGL_TEXTURE_2D ? 1 : 0);

    if (alphaTestFunc != GL_GEQUAL || alphaTestRef != 0.5f) {
      blockingError("glAlphaFunc with invalid arguments");
    }
    glUniform1f(enableAlphaTestLoc, isGL_ALPHA_TEST ? 1 : 0);
  }

  void syncBuffersPost() {
    glDisableVertexAttribArray(INDEX_POSITION);
    if (isGL_TEXTURE_COORD_ARRAY) {
      glDisableVertexAttribArray(INDEX_TEXTURE_COORD);
    }
    if (isGL_COLOR_ARRAY) {
      glDisableVertexAttribArray(INDEX_COLOR);
    }
  }

  // state managment
  void gles_adp_glEnable(GLenum cap) {
    if (cap == GL_TEXTURE_2D) {
      isGL_TEXTURE_2D = true;
      return;
    }
    if (cap == GL_ALPHA_TEST) {
      isGL_ALPHA_TEST = true;
      return;
    }
    glEnable(cap);
  };

  void gles_adp_glDisable(GLenum cap) {
    if (cap == GL_TEXTURE_2D) {
      isGL_TEXTURE_2D = false;
      return;
    }
    if (cap == GL_ALPHA_TEST) {
      isGL_ALPHA_TEST = false;
      return;
    }
    glDisable(cap);
  };

  GLboolean gles_adp_glIsEnabled(GLenum cap) {
    if (cap == GL_TEXTURE_2D) {
      return isGL_TEXTURE_2D;
    }
    if (cap == GL_VERTEX_ARRAY) {
      return isGL_VERTEX_ARRAY;
    }
    if (cap == GL_TEXTURE_COORD_ARRAY) {
      return isGL_TEXTURE_COORD_ARRAY;
    }
    if (cap == GL_NORMAL_ARRAY) {
      return isGL_NORMAL_ARRAY;
    }
    if (cap == GL_COLOR_ARRAY) {
      return isGL_COLOR_ARRAY;
    }
    if (cap == GL_ALPHA_TEST) {
      return isGL_ALPHA_TEST;
    }
    return glIsEnabled(cap);
  };

  void gles_adp_glClearColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha) {
    glClearColor(red, green, blue, alpha);
  }

  void gles_adp_glClear(GLbitfield mask) {
    glClear(mask);
  };

  void gles_adp_glColorMask(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha) {
    glColorMask(red, green, blue, alpha);
  }

  void gles_adp_glAlphaFunc(GLenum func, GLclampf ref) {
    alphaTestFunc = func;
    alphaTestRef = ref;
  };

  void gles_adp_glBlendFunc(GLenum sfactor, GLenum dfactor) {
    glBlendFunc(sfactor, dfactor);
  };

  void gles_adp_glCullFace(GLenum mode) {
    glCullFace(mode);
  };

  void gles_adp_glFrontFace(GLenum mode) {
    glFrontFace(mode);
  };

  void gles_adp_glScissor(GLint x, GLint y, GLsizei width, GLsizei height) {
    glScissor(x, y, width, height);
  }

  void gles_adp_glEnableClientState(GLenum cap) {
    if (cap == GL_VERTEX_ARRAY) {
      isGL_VERTEX_ARRAY = true;
    } else if (cap == GL_TEXTURE_COORD_ARRAY) {
      isGL_TEXTURE_COORD_ARRAY = true;
    } else if (cap == GL_NORMAL_ARRAY) {
      isGL_NORMAL_ARRAY = true;
    } else if (cap == GL_COLOR_ARRAY) {
      isGL_COLOR_ARRAY = true;
    } else {
      reportError("glEnableClientState");
    }
  };

  void gles_adp_glDisableClientState(GLenum cap) {
    if (cap == GL_VERTEX_ARRAY) {
      isGL_VERTEX_ARRAY = false;
    } else if (cap == GL_TEXTURE_COORD_ARRAY) {
      isGL_TEXTURE_COORD_ARRAY = false;
    } else if (cap == GL_NORMAL_ARRAY) {
      isGL_NORMAL_ARRAY = false;
    } else if (cap == GL_COLOR_ARRAY) {
      isGL_COLOR_ARRAY = false;
    } else {
      reportError("glDisableClientState");
    }
  };


  void gles_adp_glGetBooleanv(GLenum pname, GLboolean *params) {
    glGetBooleanv(pname, params);
  };

  void gles_adp_glGetDoublev(GLenum pname, GLdouble *params) {
    reportError("glGetDoublev");
  };

  void gles_adp_glGetFloatv(GLenum pname, GLfloat *params) {
    if (!params) return;
    glGetFloatv(pname, params);
  };

  void gles_adp_glGetIntegerv(GLenum pname, GLint *params) {
    glGetIntegerv(pname, params);
  };

  GLenum gles_adp_glGetError(void) {
    GLenum result = glGetError();
    if (lastError) result = lastError;
    lastError = 0;
    if (result) {
      int t = 0;
//      result = 0;
    }
    return result;
  };

  const GLubyte *gles_adp_glGetString(GLenum name) {
    return glGetString(name);
  };

  void gles_adp_glHint(GLenum target, GLenum mode) {
    glHint(target, mode);
  };


  void gles_adp_glClearDepth(GLclampd depth) {
    glClearDepthf(depth);
  };

  void gles_adp_glDepthFunc(GLenum func) {
    glDepthFunc(func);
  };

  void gles_adp_glDepthMask(GLboolean flag) {
    glDepthMask(flag);
  };

  void gles_adp_glDepthRange(GLclampd near_val, GLclampd far_val) {
    glDepthRangef(near_val, far_val);
  };

  void gles_adp_glMatrixMode(GLenum mode) {
    switch (mode) {
      case GL_MODELVIEW:
        currentMatrix = &modelViewMat;
        break;
      case GL_PROJECTION:
        currentMatrix = &projMat;
        break;
      case GL_TEXTURE:
        reportError("glMatrixMode(GL_TEXTURE)");
        break;
      case GL_COLOR:
        reportError("glMatrixMode(GL_COLOR)");
        break;
      default:
        lastError = GL_INVALID_ENUM;
        break;
    }
  };

  void
  gles_adp_glOrtho(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble near_val,
                   GLdouble far_val) {
    glm::mat4 toMult = glm::ortho(left, right, bottom, top, near_val, far_val);
    (*currentMatrix) *= toMult;
  }


  void
  gles_adp_glFrustum(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top,
                     GLdouble near_val,
                     GLdouble far_val) {
    glm::mat4 toMult = glm::frustum(left, right, bottom, top, near_val, far_val);
    (*currentMatrix) *= toMult;
  }


  void gles_adp_glViewport(GLint x, GLint y, GLsizei width, GLsizei height) {
    glViewport(x, y, width, height);
  }

  void gles_adp_glLoadIdentity(void) {
    *currentMatrix = glm::mat4(1);
  };

  void gles_adp_glLoadMatrixd(const GLdouble *m) {
    (*currentMatrix) = glm::make_mat4(m);
  };

  void gles_adp_glLoadMatrixf(const GLfloat *m) {
    (*currentMatrix) = glm::make_mat4(m);
  };

  void gles_adp_glMultMatrixd(const GLdouble *m) {
    glm::mat4 toMult = glm::make_mat4(m);
    (*currentMatrix) *= toMult;
  };

  void gles_adp_glMultMatrixf(const GLfloat *m) {
    glm::mat4 toMult = glm::make_mat4(m);
    (*currentMatrix) *= toMult;
  };


  void gles_adp_glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr) {
//    glVertexAttribPointer(INDEX_POSITION, size, type, GL_FALSE, stride, ptr);
    vp.size = size;
    vp.type = type;
    vp.stride = stride;
    vp.ptr = ptr;
  }

  void gles_adp_glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr) {
    cp.size = size;
    cp.type = type;
    cp.stride = stride;
    cp.ptr = ptr;
  }

  void gles_adp_glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr) {
    tp.size = size;
    tp.type = type;
    tp.stride = stride;
    tp.ptr = ptr;
  }

  void gles_adp_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    if (!enableDraws) return;
    if (!isGL_VERTEX_ARRAY) return;

    if (mode != GL_QUADS || first != 0) {
      blockingError("unimplemented mode");
    }

    // index buffer
    std::vector<uint16_t> &buffer = dummyElementBuffer;

    GLsizei vertices = count / 4 * 6;
    if (vertices > buffer.size()) {
      uint32_t i = buffer.size();
      buffer.resize(vertices);
      uint32_t faceNumber = i / 6;
      while (i < vertices) {
        buffer[i++] = faceNumber * 4 + 0;
        buffer[i++] = faceNumber * 4 + 1;
        buffer[i++] = faceNumber * 4 + 2;
        buffer[i++] = faceNumber * 4 + 2;
        buffer[i++] = faceNumber * 4 + 3;
        buffer[i++] = faceNumber * 4 + 0;
        faceNumber++;
      }
      // upload
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, INDEX_DUMMY_ELEMENT_BUFFER);
      glBufferData(GL_ELEMENT_ARRAY_BUFFER, vertices * 2, buffer.data(), GL_STATIC_DRAW);
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    }

    syncBuffers(vertices);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, INDEX_DUMMY_ELEMENT_BUFFER);
    glDrawElements(GL_TRIANGLES, vertices, GL_UNSIGNED_SHORT, 0);
//    glDrawElements(GL_LINE_STRIP, vertices, GL_UNSIGNED_SHORT, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    syncBuffersPost();
  }

  void gles_adp_glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices) {
    if (!enableDraws) return;
    if (!isGL_VERTEX_ARRAY) return;

    if (mode != GL_TRIANGLES) {
      blockingError("unimplemented mode");
    }

    uint32_t totalVertices = 0;
    if (type == GL_UNSIGNED_INT) {
      if (count > dummyIndexBuffer.size()) {
        dummyIndexBuffer.resize(count);
      }
      uint32_t *bf = (uint32_t *) indices;
      for (uint32_t i = 0; i < count; i++) {
        dummyIndexBuffer[i] = (uint16_t) bf[i];
        if (dummyIndexBuffer[i] != bf[i]) {
          blockingError("Panic!: uint16_t overflow");
        }
        if (bf[i] >= totalVertices) {
          totalVertices = bf[i] + 1;
        }
      }
      indices = (void*) dummyIndexBuffer.data();
      type = GL_UNSIGNED_SHORT;
    } else if (type == GL_UNSIGNED_SHORT) {
      uint16_t *bf = (uint16_t *) indices;
      for (uint32_t i = 0; i < count; i++) {
        if (bf[i] >= totalVertices) {
          totalVertices = bf[i] + 1;
        }
      }
    } else if (type == GL_UNSIGNED_BYTE) {
      uint8_t *bf = (uint8_t *) indices;
      for (uint32_t i = 0; i < count; i++) {
        if (bf[i] >= totalVertices) {
          totalVertices = bf[i] + 1;
        }
      }
    } else {
      blockingError("Invalid type in glDrawElements");
    }

    syncBuffers(totalVertices);
    glDrawElements(mode, count, type, indices);
    syncBuffersPost();
  }

  void gles_adp_glPixelStorei(GLenum pname, GLint param) {
    glPixelStorei(pname, param);
  };


  void
  gles_adp_glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type,
                        GLvoid *pixels) {
    if (format == GL_DEPTH_COMPONENT && type == GL_FLOAT) {
      float *it = (float*) pixels;
      for (GLsizei y = 0; y < height; y++) {
        for (GLsizei x = 0; x < height; x++) {
          *it = 1;
          it++;
        }
      }
      reportError("gles_adp_glReadPixels of depth");
      return;
    }
    glReadPixels(x, y, width, height, format, type, pixels);
  }


  void gles_adp_glClearStencil(GLint s) {
    glClearStencil(s);
  };


  void gles_adp_glGetTexEnviv(GLenum target, GLenum pname, GLint *params) {
    if (!params) return;
    if (target == GL_TEXTURE_ENV && pname == GL_TEXTURE_ENV_MODE) {
      *params = val_GL_TEXTURE_ENV_MODE;
      return;
    }
    reportError("glGetTexEnviv");
  };

  void gles_adp_glTexParameteri(GLenum target, GLenum pname, GLint param) {
    if (target == GL_TEXTURE_2D) {
      if (pname == GL_TEXTURE_WRAP_S && param == GL_CLAMP) {
        param = GL_CLAMP_TO_EDGE;
      }
      if (pname == GL_TEXTURE_WRAP_T && param == GL_CLAMP) {
        param = GL_CLAMP_TO_EDGE;
      }
    }
    glTexParameteri(target, pname, param);
  };

  void gles_adp_glTexParameterfv(GLenum target, GLenum pname, const GLfloat *params) {
    glTexParameterfv(target, pname, params);
  }

  void gles_adp_glTexParameteriv(GLenum target, GLenum pname, const GLint *params) {
    glTexParameteriv(target, pname, params);
  }

  void gles_adp_glGetTexParameterfv(GLenum target, GLenum pname, GLfloat *params) {
    glGetTexParameterfv(target, pname, params);
  }

  void gles_adp_glGetTexParameteriv(GLenum target, GLenum pname, GLint *params) {
    glGetTexParameteriv(target, pname, params);
  }

  void gles_adp_glTexImage2D(GLenum target, GLint level, GLint internalFormat, GLsizei width,
                             GLsizei height, GLint border, GLenum format, GLenum type,
                             const GLvoid *pixels) {
    // NB: internalFormat is ignored, the type should be managed by shader
    (void) internalFormat;
    glTexImage2D(target, level, format, width, height, border, format, type, pixels);
  }

  void gles_adp_glGenTextures(GLsizei n, GLuint *textures) {
    glGenTextures(n, textures);
  };

  void gles_adp_glDeleteTextures(GLsizei n, const GLuint *textures) {
    glDeleteTextures(n, textures);
  };

  void gles_adp_glBindTexture(GLenum target, GLuint texture) {
    glBindTexture(target, texture);
  };

  void
  gles_adp_glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
                           GLsizei height, GLenum format, GLenum type, const GLvoid *pixels) {
    glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels);
  }

}
             