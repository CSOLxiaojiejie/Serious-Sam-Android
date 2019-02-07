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

  void gles_adp_glClearIndex(GLfloat c) {
    reportError("glClearIndex");
  };

  void gles_adp_glClearColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha) {
    glClearColor(red, green, blue, alpha);
  }

  void gles_adp_glClear(GLbitfield mask) {
    glClear(mask);
  };

  void gles_adp_glIndexMask(GLuint mask) {
    reportError("glIndexMask");
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

  void gles_adp_glLogicOp(GLenum opcode) {
    reportError("glLogicOp");
  };

  void gles_adp_glCullFace(GLenum mode) {
    glCullFace(mode);
  };

  void gles_adp_glFrontFace(GLenum mode) {
    glFrontFace(mode);
  };

  void gles_adp_glPointSize(GLfloat size) {
    reportError("glPointSize");
  };

  void gles_adp_glLineWidth(GLfloat width) {
    reportError("glLineWidth");
  };

  void gles_adp_glLineStipple(GLint factor, GLushort pattern) {
    reportError("glLineStipple");
  };

  void gles_adp_glPolygonMode(GLenum face, GLenum mode) {
    reportError("glPolygonMode");
  };

  void gles_adp_glPolygonOffset(GLfloat factor, GLfloat units) {
    reportError("glPolygonOffset");
  };

  void gles_adp_glPolygonStipple(const GLubyte *mask) {
    reportError("glPolygonStipple");
  };

  void gles_adp_glGetPolygonStipple(GLubyte *mask) {
    reportError("glGetPolygonStipple");
  };

  void gles_adp_glEdgeFlag(GLboolean flag) {
    reportError("glEdgeFlag");
  };

  void gles_adp_glEdgeFlagv(const GLboolean *flag) {
    reportError("glEdgeFlagv");
  };

  void gles_adp_glScissor(GLint x, GLint y, GLsizei width, GLsizei height) {
    glScissor(x, y, width, height);
  }

  void gles_adp_glClipPlane(GLenum plane, const GLdouble *equation) {
    reportError("glClipPlane");
  };

  void gles_adp_glGetClipPlane(GLenum plane, GLdouble *equation) {
    reportError("glGetClipPlane");
  };

  void gles_adp_glDrawBuffer(GLenum mode) {
    reportError("glDrawBuffer");
  };

  void gles_adp_glReadBuffer(GLenum mode) {
    reportError("glReadBuffer");
  };


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


  void gles_adp_glPushAttrib(GLbitfield mask) {
    reportError("glPushAttrib");
  };

  void gles_adp_glPopAttrib(void) {
    reportError("glPopAttrib");
  };


  void gles_adp_glPushClientAttrib(GLbitfield mask) {
    reportError("glPushClientAttrib");
  };

  void gles_adp_glPopClientAttrib(void) {
    reportError("glPopClientAttrib");
  };


  GLint gles_adp_glRenderMode(GLenum mode) {
    reportError("glRenderMode");
    return 0;
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

  void gles_adp_glFinish(void) {
    reportError("glFinish");
  };

  void gles_adp_glFlush(void) {
    reportError("glFlush");
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


  void gles_adp_glClearAccum(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha) {
    reportError("glClearAccum");
  }

  void gles_adp_glAccum(GLenum op, GLfloat value) {
    reportError("glAccum");
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

  void gles_adp_glPushMatrix(void) {
    reportError("glPushMatrix");
  };

  void gles_adp_glPopMatrix(void) {
    reportError("glPopMatrix");
  };

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

  void gles_adp_glRotated(GLdouble angle, GLdouble x, GLdouble y, GLdouble z) {
    reportError("glRotated");
  }

  void gles_adp_glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z) {
    reportError("glRotatef");
  }

  void gles_adp_glScaled(GLdouble x, GLdouble y, GLdouble z) {
    reportError("glScaled");
  };

  void gles_adp_glScalef(GLfloat x, GLfloat y, GLfloat z) {
    reportError("glScalef");
  };

  void gles_adp_glTranslated(GLdouble x, GLdouble y, GLdouble z) {
    reportError("glTranslated");
  };

  void gles_adp_glTranslatef(GLfloat x, GLfloat y, GLfloat z) {
    reportError("glTranslatef");
  };

  GLboolean gles_adp_glIsList(GLuint list) {
    reportError("glIsList");
    return 0;
  };

  void gles_adp_glDeleteLists(GLuint list, GLsizei range) {
    reportError("glDeleteLists");
  };

  GLuint gles_adp_glGenLists(GLsizei range) {
    reportError("glGenLists");
    return 0;
  };

  void gles_adp_glNewList(GLuint list, GLenum mode) {
    reportError("glNewList");
  };

  void gles_adp_glEndList(void) {
    reportError("glEndList");
  };

  void gles_adp_glCallList(GLuint list) {
    reportError("glCallList");
  };

  void gles_adp_glCallLists(GLsizei n, GLenum type, const GLvoid *lists) {
    reportError("glCallLists");
  }

  void gles_adp_glListBase(GLuint base) {
    reportError("glListBase");
  };


  void gles_adp_glBegin(GLenum mode) {
    reportError("glBegin");
  };

  void gles_adp_glEnd(void) {
    reportError("glEnd");
  };


  void gles_adp_glVertex2d(GLdouble x, GLdouble y) {
    reportError("glVertex2d");
  };

  void gles_adp_glVertex2f(GLfloat x, GLfloat y) {
    reportError("glVertex2f");
  };

  void gles_adp_glVertex2i(GLint x, GLint y) {
    reportError("glVertex2i");
  };

  void gles_adp_glVertex2s(GLshort x, GLshort y) {
    reportError("glVertex2s");
  };

  void gles_adp_glVertex3d(GLdouble x, GLdouble y, GLdouble z) {
    reportError("glVertex3d");
  };

  void gles_adp_glVertex3f(GLfloat x, GLfloat y, GLfloat z) {
    reportError("glVertex3f");
  };

  void gles_adp_glVertex3i(GLint x, GLint y, GLint z) {
    reportError("glVertex3i");
  };

  void gles_adp_glVertex3s(GLshort x, GLshort y, GLshort z) {
    reportError("glVertex3s");
  };

  void gles_adp_glVertex4d(GLdouble x, GLdouble y, GLdouble z, GLdouble w) {
    reportError("glVertex4d");
  };

  void gles_adp_glVertex4f(GLfloat x, GLfloat y, GLfloat z, GLfloat w) {
    reportError("glVertex4f");
  };

  void gles_adp_glVertex4i(GLint x, GLint y, GLint z, GLint w) {
    reportError("glVertex4i");
  };

  void gles_adp_glVertex4s(GLshort x, GLshort y, GLshort z, GLshort w) {
    reportError("glVertex4s");
  };

  void gles_adp_glVertex2dv(const GLdouble *v) {
    reportError("glVertex2dv");
  };

  void gles_adp_glVertex2fv(const GLfloat *v) {
    reportError("glVertex2fv");
  };

  void gles_adp_glVertex2iv(const GLint *v) {
    reportError("glVertex2iv");
  };

  void gles_adp_glVertex2sv(const GLshort *v) {
    reportError("glVertex2sv");
  };

  void gles_adp_glVertex3dv(const GLdouble *v) {
    reportError("glVertex3dv");
  };

  void gles_adp_glVertex3fv(const GLfloat *v) {
    reportError("glVertex3fv");
  };

  void gles_adp_glVertex3iv(const GLint *v) {
    reportError("glVertex3iv");
  };

  void gles_adp_glVertex3sv(const GLshort *v) {
    reportError("glVertex3sv");
  };

  void gles_adp_glVertex4dv(const GLdouble *v) {
    reportError("glVertex4dv");
  };

  void gles_adp_glVertex4fv(const GLfloat *v) {
    reportError("glVertex4fv");
  };

  void gles_adp_glVertex4iv(const GLint *v) {
    reportError("glVertex4iv");
  };

  void gles_adp_glVertex4sv(const GLshort *v) {
    reportError("glVertex4sv");
  };


  void gles_adp_glNormal3b(GLbyte nx, GLbyte ny, GLbyte nz) {
    reportError("glNormal3b");
  };

  void gles_adp_glNormal3d(GLdouble nx, GLdouble ny, GLdouble nz) {
    reportError("glNormal3d");
  };

  void gles_adp_glNormal3f(GLfloat nx, GLfloat ny, GLfloat nz) {
    reportError("glNormal3f");
  };

  void gles_adp_glNormal3i(GLint nx, GLint ny, GLint nz) {
    reportError("glNormal3i");
  };

  void gles_adp_glNormal3s(GLshort nx, GLshort ny, GLshort nz) {
    reportError("glNormal3s");
  };

  void gles_adp_glNormal3bv(const GLbyte *v) {
    reportError("glNormal3bv");
  };

  void gles_adp_glNormal3dv(const GLdouble *v) {
    reportError("glNormal3dv");
  };

  void gles_adp_glNormal3fv(const GLfloat *v) {
    reportError("glNormal3fv");
  };

  void gles_adp_glNormal3iv(const GLint *v) {
    reportError("glNormal3iv");
  };

  void gles_adp_glNormal3sv(const GLshort *v) {
    reportError("glNormal3sv");
  };


  void gles_adp_glIndexd(GLdouble c) {
    reportError("glIndexd");
  };

  void gles_adp_glIndexf(GLfloat c) {
    reportError("glIndexf");
  };

  void gles_adp_glIndexi(GLint c) {
    reportError("glIndexi");
  };

  void gles_adp_glIndexs(GLshort c) {
    reportError("glIndexs");
  };

  void gles_adp_glIndexub(GLubyte c) {
    reportError("glIndexub");
  };

  void gles_adp_glIndexdv(const GLdouble *c) {
    reportError("glIndexdv");
  };

  void gles_adp_glIndexfv(const GLfloat *c) {
    reportError("glIndexfv");
  };

  void gles_adp_glIndexiv(const GLint *c) {
    reportError("glIndexiv");
  };

  void gles_adp_glIndexsv(const GLshort *c) {
    reportError("glIndexsv");
  };

  void gles_adp_glIndexubv(const GLubyte *c) {
    reportError("glIndexubv");
  };

  void gles_adp_glColor3b(GLbyte red, GLbyte green, GLbyte blue) {
    reportError("glColor3b");
  };

  void gles_adp_glColor3d(GLdouble red, GLdouble green, GLdouble blue) {
    reportError("glColor3d");
  };

  void gles_adp_glColor3f(GLfloat red, GLfloat green, GLfloat blue) {
    reportError("glColor3f");
  };

  void gles_adp_glColor3i(GLint red, GLint green, GLint blue) {
    reportError("glColor3i");
  };

  void gles_adp_glColor3s(GLshort red, GLshort green, GLshort blue) {
    reportError("glColor3s");
  };

  void gles_adp_glColor3ub(GLubyte red, GLubyte green, GLubyte blue) {
    reportError("glColor3ub");
  };

  void gles_adp_glColor3ui(GLuint red, GLuint green, GLuint blue) {
    reportError("glColor3ui");
  };

  void gles_adp_glColor3us(GLushort red, GLushort green, GLushort blue) {
    reportError("glColor3us");
  };

  void gles_adp_glColor4b(GLbyte red, GLbyte green, GLbyte blue, GLbyte alpha) {
    reportError("glColor4b");
  }

  void gles_adp_glColor4d(GLdouble red, GLdouble green, GLdouble blue, GLdouble alpha) {
    reportError("glColor4d");
  }

  void gles_adp_glColor4f(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha) {
    reportError("glColor4f");
  }

  void gles_adp_glColor4i(GLint red, GLint green, GLint blue, GLint alpha) {
    reportError("glColor4i");
  }

  void gles_adp_glColor4s(GLshort red, GLshort green, GLshort blue, GLshort alpha) {
    reportError("glColor4s");
  }

  void gles_adp_glColor4ub(GLubyte red, GLubyte green, GLubyte blue, GLubyte alpha) {
    reportError("glColor4ub");
  }

  void gles_adp_glColor4ui(GLuint red, GLuint green, GLuint blue, GLuint alpha) {
    reportError("glColor4ui");
  }

  void gles_adp_glColor4us(GLushort red, GLushort green, GLushort blue, GLushort alpha) {
    reportError("glColor4us");
  }


  void gles_adp_glColor3bv(const GLbyte *v) {
    reportError("glColor3bv");
  };

  void gles_adp_glColor3dv(const GLdouble *v) {
    reportError("glColor3dv");
  };

  void gles_adp_glColor3fv(const GLfloat *v) {
    reportError("glColor3fv");
  };

  void gles_adp_glColor3iv(const GLint *v) {
    reportError("glColor3iv");
  };

  void gles_adp_glColor3sv(const GLshort *v) {
    reportError("glColor3sv");
  };

  void gles_adp_glColor3ubv(const GLubyte *v) {
    reportError("glColor3ubv");
  };

  void gles_adp_glColor3uiv(const GLuint *v) {
    reportError("glColor3uiv");
  };

  void gles_adp_glColor3usv(const GLushort *v) {
    reportError("glColor3usv");
  };

  void gles_adp_glColor4bv(const GLbyte *v) {
    reportError("glColor4bv");
  };

  void gles_adp_glColor4dv(const GLdouble *v) {
    reportError("glColor4dv");
  };

  void gles_adp_glColor4fv(const GLfloat *v) {
    reportError("glColor4fv");
  };

  void gles_adp_glColor4iv(const GLint *v) {
    reportError("glColor4iv");
  };

  void gles_adp_glColor4sv(const GLshort *v) {
    reportError("glColor4sv");
  };

  void gles_adp_glColor4ubv(const GLubyte *v) {
    reportError("glColor4ubv");
  };

  void gles_adp_glColor4uiv(const GLuint *v) {
    reportError("glColor4uiv");
  };

  void gles_adp_glColor4usv(const GLushort *v) {
    reportError("glColor4usv");
  };


  void gles_adp_glTexCoord1d(GLdouble s) {
    reportError("glTexCoord1d");
  };

  void gles_adp_glTexCoord1f(GLfloat s) {
    reportError("glTexCoord1f");
  };

  void gles_adp_glTexCoord1i(GLint s) {
    reportError("glTexCoord1i");
  };

  void gles_adp_glTexCoord1s(GLshort s) {
    reportError("glTexCoord1s");
  };

  void gles_adp_glTexCoord2d(GLdouble s, GLdouble t) {
    reportError("glTexCoord2d");
  };

  void gles_adp_glTexCoord2f(GLfloat s, GLfloat t) {
    reportError("glTexCoord2f");
  };

  void gles_adp_glTexCoord2i(GLint s, GLint t) {
    reportError("glTexCoord2i");
  };

  void gles_adp_glTexCoord2s(GLshort s, GLshort t) {
    reportError("glTexCoord2s");
  };

  void gles_adp_glTexCoord3d(GLdouble s, GLdouble t, GLdouble r) {
    reportError("glTexCoord3d");
  };

  void gles_adp_glTexCoord3f(GLfloat s, GLfloat t, GLfloat r) {
    reportError("glTexCoord3f");
  };

  void gles_adp_glTexCoord3i(GLint s, GLint t, GLint r) {
    reportError("glTexCoord3i");
  };

  void gles_adp_glTexCoord3s(GLshort s, GLshort t, GLshort r) {
    reportError("glTexCoord3s");
  };

  void gles_adp_glTexCoord4d(GLdouble s, GLdouble t, GLdouble r, GLdouble q) {
    reportError("glTexCoord4d");
  };

  void gles_adp_glTexCoord4f(GLfloat s, GLfloat t, GLfloat r, GLfloat q) {
    reportError("glTexCoord4f");
  };

  void gles_adp_glTexCoord4i(GLint s, GLint t, GLint r, GLint q) {
    reportError("glTexCoord4i");
  };

  void gles_adp_glTexCoord4s(GLshort s, GLshort t, GLshort r, GLshort q) {
    reportError("glTexCoord4s");
  };

  void gles_adp_glTexCoord1dv(const GLdouble *v) {
    reportError("glTexCoord1dv");
  };

  void gles_adp_glTexCoord1fv(const GLfloat *v) {
    reportError("glTexCoord1fv");
  };

  void gles_adp_glTexCoord1iv(const GLint *v) {
    reportError("glTexCoord1iv");
  };

  void gles_adp_glTexCoord1sv(const GLshort *v) {
    reportError("glTexCoord1sv");
  };

  void gles_adp_glTexCoord2dv(const GLdouble *v) {
    reportError("glTexCoord2dv");
  };

  void gles_adp_glTexCoord2fv(const GLfloat *v) {
    reportError("glTexCoord2fv");
  };

  void gles_adp_glTexCoord2iv(const GLint *v) {
    reportError("glTexCoord2iv");
  };

  void gles_adp_glTexCoord2sv(const GLshort *v) {
    reportError("glTexCoord2sv");
  };

  void gles_adp_glTexCoord3dv(const GLdouble *v) {
    reportError("glTexCoord3dv");
  };

  void gles_adp_glTexCoord3fv(const GLfloat *v) {
    reportError("glTexCoord3fv");
  };

  void gles_adp_glTexCoord3iv(const GLint *v) {
    reportError("glTexCoord3iv");
  };

  void gles_adp_glTexCoord3sv(const GLshort *v) {
    reportError("glTexCoord3sv");
  };

  void gles_adp_glTexCoord4dv(const GLdouble *v) {
    reportError("glTexCoord4dv");
  };

  void gles_adp_glTexCoord4fv(const GLfloat *v) {
    reportError("glTexCoord4fv");
  };

  void gles_adp_glTexCoord4iv(const GLint *v) {
    reportError("glTexCoord4iv");
  };

  void gles_adp_glTexCoord4sv(const GLshort *v) {
    reportError("glTexCoord4sv");
  };


  void gles_adp_glRasterPos2d(GLdouble x, GLdouble y) {
    reportError("glRasterPos2d");
  };

  void gles_adp_glRasterPos2f(GLfloat x, GLfloat y) {
    reportError("glRasterPos2f");
  };

  void gles_adp_glRasterPos2i(GLint x, GLint y) {
    reportError("glRasterPos2i");
  };

  void gles_adp_glRasterPos2s(GLshort x, GLshort y) {
    reportError("glRasterPos2s");
  };

  void gles_adp_glRasterPos3d(GLdouble x, GLdouble y, GLdouble z) {
    reportError("glRasterPos3d");
  };

  void gles_adp_glRasterPos3f(GLfloat x, GLfloat y, GLfloat z) {
    reportError("glRasterPos3f");
  };

  void gles_adp_glRasterPos3i(GLint x, GLint y, GLint z) {
    reportError("glRasterPos3i");
  };

  void gles_adp_glRasterPos3s(GLshort x, GLshort y, GLshort z) {
    reportError("glRasterPos3s");
  };

  void gles_adp_glRasterPos4d(GLdouble x, GLdouble y, GLdouble z, GLdouble w) {
    reportError("glRasterPos4d");
  };

  void gles_adp_glRasterPos4f(GLfloat x, GLfloat y, GLfloat z, GLfloat w) {
    reportError("glRasterPos4f");
  };

  void gles_adp_glRasterPos4i(GLint x, GLint y, GLint z, GLint w) {
    reportError("glRasterPos4i");
  };

  void gles_adp_glRasterPos4s(GLshort x, GLshort y, GLshort z, GLshort w) {
    reportError("glRasterPos4s");
  };

  void gles_adp_glRasterPos2dv(const GLdouble *v) {
    reportError("glRasterPos2dv");
  };

  void gles_adp_glRasterPos2fv(const GLfloat *v) {
    reportError("glRasterPos2fv");
  };

  void gles_adp_glRasterPos2iv(const GLint *v) {
    reportError("glRasterPos2iv");
  };

  void gles_adp_glRasterPos2sv(const GLshort *v) {
    reportError("glRasterPos2sv");
  };

  void gles_adp_glRasterPos3dv(const GLdouble *v) {
    reportError("glRasterPos3dv");
  };

  void gles_adp_glRasterPos3fv(const GLfloat *v) {
    reportError("glRasterPos3fv");
  };

  void gles_adp_glRasterPos3iv(const GLint *v) {
    reportError("glRasterPos3iv");
  };

  void gles_adp_glRasterPos3sv(const GLshort *v) {
    reportError("glRasterPos3sv");
  };

  void gles_adp_glRasterPos4dv(const GLdouble *v) {
    reportError("glRasterPos4dv");
  };

  void gles_adp_glRasterPos4fv(const GLfloat *v) {
    reportError("glRasterPos4fv");
  };

  void gles_adp_glRasterPos4iv(const GLint *v) {
    reportError("glRasterPos4iv");
  };

  void gles_adp_glRasterPos4sv(const GLshort *v) {
    reportError("glRasterPos4sv");
  };


  void gles_adp_glRectd(GLdouble x1, GLdouble y1, GLdouble x2, GLdouble y2) {
    reportError("glRectd");
  };

  void gles_adp_glRectf(GLfloat x1, GLfloat y1, GLfloat x2, GLfloat y2) {
    reportError("glRectf");
  };

  void gles_adp_glRecti(GLint x1, GLint y1, GLint x2, GLint y2) {
    reportError("glRecti");
  };

  void gles_adp_glRects(GLshort x1, GLshort y1, GLshort x2, GLshort y2) {
    reportError("glRects");
  };


  void gles_adp_glRectdv(const GLdouble *v1, const GLdouble *v2) {
    reportError("glRectdv");
  };

  void gles_adp_glRectfv(const GLfloat *v1, const GLfloat *v2) {
    reportError("glRectfv");
  };

  void gles_adp_glRectiv(const GLint *v1, const GLint *v2) {
    reportError("glRectiv");
  };

  void gles_adp_glRectsv(const GLshort *v1, const GLshort *v2) {
    reportError("glRectsv");
  };


  void gles_adp_glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr) {
//    glVertexAttribPointer(INDEX_POSITION, size, type, GL_FALSE, stride, ptr);
    vp.size = size;
    vp.type = type;
    vp.stride = stride;
    vp.ptr = ptr;
  }

  void gles_adp_glNormalPointer(GLenum type, GLsizei stride, const GLvoid *ptr) {
    reportError("glNormalPointer");
  }

  void gles_adp_glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr) {
    cp.size = size;
    cp.type = type;
    cp.stride = stride;
    cp.ptr = ptr;
  }

  void gles_adp_glIndexPointer(GLenum type, GLsizei stride, const GLvoid *ptr) {
    reportError("glIndexPointer");
  }

  void gles_adp_glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *ptr) {
    tp.size = size;
    tp.type = type;
    tp.stride = stride;
    tp.ptr = ptr;
  }

  void gles_adp_glEdgeFlagPointer(GLsizei stride, const GLboolean *ptr) {
    reportError("glEdgeFlagPointer");
  }

  void gles_adp_glGetPointerv(GLenum pname, void **params) {
    reportError("glGetPointerv");
  };

  void gles_adp_glArrayElement(GLint i) {
    reportError("glArrayElement");
  };

  void gles_adp_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
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

  void gles_adp_glInterleavedArrays(GLenum format, GLsizei stride, const GLvoid *pointer) {
    reportError("glInterleavedArrays");
  }


  void gles_adp_glShadeModel(GLenum mode) {
    reportError("glShadeModel");
  };

  void gles_adp_glLightf(GLenum light, GLenum pname, GLfloat param) {
    reportError("glLightf");
  };

  void gles_adp_glLighti(GLenum light, GLenum pname, GLint param) {
    reportError("glLighti");
  };

  void gles_adp_glLightfv(GLenum light, GLenum pname, const GLfloat *params) {
    reportError("glLightfv");
  }

  void gles_adp_glLightiv(GLenum light, GLenum pname, const GLint *params) {
    reportError("glLightiv");
  }

  void gles_adp_glGetLightfv(GLenum light, GLenum pname, GLfloat *params) {
    reportError("glGetLightfv");
  }

  void gles_adp_glGetLightiv(GLenum light, GLenum pname, GLint *params) {
    reportError("glGetLightiv");
  }

  void gles_adp_glLightModelf(GLenum pname, GLfloat param) {
    reportError("glLightModelf");
  };

  void gles_adp_glLightModeli(GLenum pname, GLint param) {
    reportError("glLightModeli");
  };

  void gles_adp_glLightModelfv(GLenum pname, const GLfloat *params) {
    reportError("glLightModelfv");
  };

  void gles_adp_glLightModeliv(GLenum pname, const GLint *params) {
    reportError("glLightModeliv");
  };

  void gles_adp_glMaterialf(GLenum face, GLenum pname, GLfloat param) {
    reportError("glMaterialf");
  };

  void gles_adp_glMateriali(GLenum face, GLenum pname, GLint param) {
    reportError("glMateriali");
  };

  void gles_adp_glMaterialfv(GLenum face, GLenum pname, const GLfloat *params) {
    reportError("glMaterialfv");
  };

  void gles_adp_glMaterialiv(GLenum face, GLenum pname, const GLint *params) {
    reportError("glMaterialiv");
  };

  void gles_adp_glGetMaterialfv(GLenum face, GLenum pname, GLfloat *params) {
    reportError("glGetMaterialfv");
  };

  void gles_adp_glGetMaterialiv(GLenum face, GLenum pname, GLint *params) {
    reportError("glGetMaterialiv");
  };

  void gles_adp_glColorMaterial(GLenum face, GLenum mode) {
    reportError("glColorMaterial");
  };
# 463 "gl_impl.cpp"

  void gles_adp_glPixelZoom(GLfloat xfactor, GLfloat yfactor) {
    reportError("glPixelZoom");
  };

  void gles_adp_glPixelStoref(GLenum pname, GLfloat param) {
    reportError("glPixelStoref");
  };

  void gles_adp_glPixelStorei(GLenum pname, GLint param) {
    glPixelStorei(pname, param);
  };

  void gles_adp_glPixelTransferf(GLenum pname, GLfloat param) {
    reportError("glPixelTransferf");
  };

  void gles_adp_glPixelTransferi(GLenum pname, GLint param) {
    reportError("glPixelTransferi");
  };

  void gles_adp_glPixelMapfv(GLenum map, GLint mapsize, const GLfloat *values) {
    reportError("glPixelMapfv");
  }

  void gles_adp_glPixelMapuiv(GLenum map, GLint mapsize, const GLuint *values) {
    reportError("glPixelMapuiv");
  }

  void gles_adp_glPixelMapusv(GLenum map, GLint mapsize, const GLushort *values) {
    reportError("glPixelMapusv");
  }

  void gles_adp_glGetPixelMapfv(GLenum map, GLfloat *values) {
    reportError("glGetPixelMapfv");
  };

  void gles_adp_glGetPixelMapuiv(GLenum map, GLuint *values) {
    reportError("glGetPixelMapuiv");
  };

  void gles_adp_glGetPixelMapusv(GLenum map, GLushort *values) {
    reportError("glGetPixelMapusv");
  };

  void gles_adp_glBitmap(GLsizei width, GLsizei height, GLfloat xorig, GLfloat yorig, GLfloat xmove,
                         GLfloat ymove, const GLubyte *bitmap) {
    reportError("glBitmap");
  }


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


  void gles_adp_glDrawPixels(GLsizei width, GLsizei height, GLenum format, GLenum type,
                             const GLvoid *pixels) {
    reportError("glDrawPixels");
  }


  void gles_adp_glCopyPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum type) {
    reportError("glCopyPixels");
  }


  void gles_adp_glStencilFunc(GLenum func, GLint ref, GLuint mask) {
    reportError("glStencilFunc");
  };

  void gles_adp_glStencilMask(GLuint mask) {
    reportError("glStencilMask");
  };

  void gles_adp_glStencilOp(GLenum fail, GLenum zfail, GLenum zpass) {
    reportError("glStencilOp");
  };

  void gles_adp_glClearStencil(GLint s) {
    glClearStencil(s);
  };


  void gles_adp_glTexGend(GLenum coord, GLenum pname, GLdouble param) {
    reportError("glTexGend");
  };

  void gles_adp_glTexGenf(GLenum coord, GLenum pname, GLfloat param) {
    reportError("glTexGenf");
  };

  void gles_adp_glTexGeni(GLenum coord, GLenum pname, GLint param) {
    reportError("glTexGeni");
  };

  void gles_adp_glTexGendv(GLenum coord, GLenum pname, const GLdouble *params) {
    reportError("glTexGendv");
  };

  void gles_adp_glTexGenfv(GLenum coord, GLenum pname, const GLfloat *params) {
    reportError("glTexGenfv");
  };

  void gles_adp_glTexGeniv(GLenum coord, GLenum pname, const GLint *params) {
    reportError("glTexGeniv");
  };

  void gles_adp_glGetTexGendv(GLenum coord, GLenum pname, GLdouble *params) {
    reportError("glGetTexGendv");
  };

  void gles_adp_glGetTexGenfv(GLenum coord, GLenum pname, GLfloat *params) {
    reportError("glGetTexGenfv");
  };

  void gles_adp_glGetTexGeniv(GLenum coord, GLenum pname, GLint *params) {
    reportError("glGetTexGeniv");
  };


  void gles_adp_glTexEnvf(GLenum target, GLenum pname, GLfloat param) {
    reportError("glTexEnvf");
  };

  void gles_adp_glTexEnvi(GLenum target, GLenum pname, GLint param) {
    reportError("glTexEnvi");
  };

  void gles_adp_glTexEnvfv(GLenum target, GLenum pname, const GLfloat *params) {
    reportError("glTexEnvfv");
  };

  void gles_adp_glTexEnviv(GLenum target, GLenum pname, const GLint *params) {
    reportError("glTexEnviv");
  };

  void gles_adp_glGetTexEnvfv(GLenum target, GLenum pname, GLfloat *params) {
    reportError("glGetTexEnvfv");
  };

  void gles_adp_glGetTexEnviv(GLenum target, GLenum pname, GLint *params) {
    if (!params) return;
    if (target == GL_TEXTURE_ENV && pname == GL_TEXTURE_ENV_MODE) {
      *params = val_GL_TEXTURE_ENV_MODE;
      return;
    }
    reportError("glGetTexEnviv");
  };


  void gles_adp_glTexParameterf(GLenum target, GLenum pname, GLfloat param) {
    reportError("glTexParameterf");
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

  void
  gles_adp_glGetTexLevelParameterfv(GLenum target, GLint level, GLenum pname, GLfloat *params) {
    reportError("glGetTexLevelParameterfv");
  }

  void gles_adp_glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint *params) {
    // eglGetProcAddress("glGetTexLevelParameteriv") ??
    reportError("glGetTexLevelParameteriv");
  }


  void
  gles_adp_glTexImage1D(GLenum target, GLint level, GLint internalFormat, GLsizei width,
                        GLint border, GLenum format, GLenum type, const GLvoid *pixels) {
    reportError("glTexImage1D");
  }


  void gles_adp_glTexImage2D(GLenum target, GLint level, GLint internalFormat, GLsizei width,
                             GLsizei height, GLint border, GLenum format, GLenum type,
                             const GLvoid *pixels) {
    // NB: internalFormat is ignored, the type should be managed by shader
    (void) internalFormat;
    glTexImage2D(target, level, format, width, height, border, format, type, pixels);
  }


  void
  gles_adp_glGetTexImage(GLenum target, GLint level, GLenum format, GLenum type, GLvoid *pixels) {
    reportError("glGetTexImage");
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
  gles_adp_glPrioritizeTextures(GLsizei n, const GLuint *textures, const GLclampf *priorities) {
    reportError("glPrioritizeTextures");
  }


  GLboolean
  gles_adp_glAreTexturesResident(GLsizei n, const GLuint *textures, GLboolean *residences) {
    reportError("glAreTexturesResident");
  }


  GLboolean gles_adp_glIsTexture(GLuint texture) {
    reportError("glIsTexture");
  };


  void
  gles_adp_glTexSubImage1D(GLenum target, GLint level, GLint xoffset, GLsizei width, GLenum format,
                           GLenum type, const GLvoid *pixels) {
    reportError("glTexSubImage1D");
  }


  void
  gles_adp_glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
                           GLsizei height, GLenum format, GLenum type, const GLvoid *pixels) {
    glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels);
  }


  void
  gles_adp_glCopyTexImage1D(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y,
                            GLsizei width, GLint border) {
    reportError("glCopyTexImage1D");
  }


  void
  gles_adp_glCopyTexImage2D(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y,
                            GLsizei width, GLsizei height, GLint border) {
    reportError("glCopyTexImage2D");
  }


  void gles_adp_glCopyTexSubImage1D(GLenum target, GLint level, GLint xoffset, GLint x, GLint y,
                                    GLsizei width) {
    reportError("glCopyTexSubImage1D");
  }


  void
  gles_adp_glCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x,
                               GLint y, GLsizei width, GLsizei height) {
    reportError("glCopyTexSubImage2D");
  }


  void gles_adp_glMap1d(GLenum target, GLdouble u1, GLdouble u2, GLint stride, GLint order,
                        const GLdouble *points) {
    reportError("glMap1d");
  }

  void gles_adp_glMap1f(GLenum target, GLfloat u1, GLfloat u2, GLint stride, GLint order,
                        const GLfloat *points) {
    reportError("glMap1f");
  }


  void
  gles_adp_glMap2d(GLenum target, GLdouble u1, GLdouble u2, GLint ustride, GLint uorder,
                   GLdouble v1,
                   GLdouble v2, GLint vstride, GLint vorder, const GLdouble *points) {
    reportError("glMap2d");
  }


  void
  gles_adp_glMap2f(GLenum target, GLfloat u1, GLfloat u2, GLint ustride, GLint uorder, GLfloat v1,
                   GLfloat v2, GLint vstride, GLint vorder, const GLfloat *points) {
    reportError("glMap2f");
  }


  void gles_adp_glGetMapdv(GLenum target, GLenum query, GLdouble *v) {
    reportError("glGetMapdv");
  };

  void gles_adp_glGetMapfv(GLenum target, GLenum query, GLfloat *v) {
    reportError("glGetMapfv");
  };

  void gles_adp_glGetMapiv(GLenum target, GLenum query, GLint *v) {
    reportError("glGetMapiv");
  };

  void gles_adp_glEvalCoord1d(GLdouble u) {
    reportError("glEvalCoord1d");
  };

  void gles_adp_glEvalCoord1f(GLfloat u) {
    reportError("glEvalCoord1f");
  };

  void gles_adp_glEvalCoord1dv(const GLdouble *u) {
    reportError("glEvalCoord1dv");
  };

  void gles_adp_glEvalCoord1fv(const GLfloat *u) {
    reportError("glEvalCoord1fv");
  };

  void gles_adp_glEvalCoord2d(GLdouble u, GLdouble v) {
    reportError("glEvalCoord2d");
  };

  void gles_adp_glEvalCoord2f(GLfloat u, GLfloat v) {
    reportError("glEvalCoord2f");
  };

  void gles_adp_glEvalCoord2dv(const GLdouble *u) {
    reportError("glEvalCoord2dv");
  };

  void gles_adp_glEvalCoord2fv(const GLfloat *u) {
    reportError("glEvalCoord2fv");
  };

  void gles_adp_glMapGrid1d(GLint un, GLdouble u1, GLdouble u2) {
    reportError("glMapGrid1d");
  };

  void gles_adp_glMapGrid1f(GLint un, GLfloat u1, GLfloat u2) {
    reportError("glMapGrid1f");
  };

  void
  gles_adp_glMapGrid2d(GLint un, GLdouble u1, GLdouble u2, GLint vn, GLdouble v1, GLdouble v2) {
    reportError("glMapGrid2d");
  }

  void gles_adp_glMapGrid2f(GLint un, GLfloat u1, GLfloat u2, GLint vn, GLfloat v1, GLfloat v2) {
    reportError("glMapGrid2f");
  }

  void gles_adp_glEvalPoint1(GLint i) {
    reportError("glEvalPoint1");
  };

  void gles_adp_glEvalPoint2(GLint i, GLint j) {
    reportError("glEvalPoint2");
  };

  void gles_adp_glEvalMesh1(GLenum mode, GLint i1, GLint i2) {
    reportError("glEvalMesh1");
  };

  void gles_adp_glEvalMesh2(GLenum mode, GLint i1, GLint i2, GLint j1, GLint j2) {
    reportError("glEvalMesh2");
  };


  void gles_adp_glFogf(GLenum pname, GLfloat param) {
    reportError("glFogf");
  };

  void gles_adp_glFogi(GLenum pname, GLint param) {
    reportError("glFogi");
  };

  void gles_adp_glFogfv(GLenum pname, const GLfloat *params) {
    reportError("glFogfv");
  };

  void gles_adp_glFogiv(GLenum pname, const GLint *params) {
    reportError("glFogiv");
  };


  void gles_adp_glFeedbackBuffer(GLsizei size, GLenum type, GLfloat *buffer) {
    reportError("glFeedbackBuffer");
  };

  void gles_adp_glPassThrough(GLfloat token) {
    reportError("glPassThrough");
  };

  void gles_adp_glSelectBuffer(GLsizei size, GLuint *buffer) {
    reportError("glSelectBuffer");
  };

  void gles_adp_glInitNames(void) {
    reportError("glInitNames");
  };

  void gles_adp_glLoadName(GLuint name) {
    reportError("glLoadName");
  };

  void gles_adp_glPushName(GLuint name) {
    reportError("glPushName");
  };

  void gles_adp_glPopName(void) {
    reportError("glPopName");
  };
# 731 "gl_impl.cpp"

  void gles_adp_glBlendEquationEXT(GLenum mode) {
    reportError("glBlendEquationEXT");
  };


  void gles_adp_glBlendColorEXT(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha) {
    reportError("glBlendColorEXT");
  }


  void gles_adp_glPolygonOffsetEXT(GLfloat factor, GLfloat bias) {
    reportError("glPolygonOffsetEXT");
  };


  void gles_adp_glVertexPointerEXT(GLint size, GLenum type, GLsizei stride, GLsizei count,
                                   const GLvoid *ptr) {
    reportError("glVertexPointerEXT");
  }


  void gles_adp_glNormalPointerEXT(GLenum type, GLsizei stride, GLsizei count, const GLvoid *ptr) {
    reportError("glNormalPointerEXT");
  }

  void gles_adp_glColorPointerEXT(GLint size, GLenum type, GLsizei stride, GLsizei count,
                                  const GLvoid *ptr) {
    reportError("glColorPointerEXT");
  }


  void gles_adp_glIndexPointerEXT(GLenum type, GLsizei stride, GLsizei count, const GLvoid *ptr) {
    reportError("glIndexPointerEXT");
  }

  void gles_adp_glTexCoordPointerEXT(GLint size, GLenum type, GLsizei stride, GLsizei count,
                                     const GLvoid *ptr) {
    reportError("glTexCoordPointerEXT");
  }


  void gles_adp_glEdgeFlagPointerEXT(GLsizei stride, GLsizei count, const GLboolean *ptr) {
    reportError("glEdgeFlagPointerEXT");
  }

  void gles_adp_glGetPointervEXT(GLenum pname, void **params) {
    reportError("glGetPointervEXT");
  };

  void gles_adp_glArrayElementEXT(GLint i) {
    reportError("glArrayElementEXT");
  };

  void gles_adp_glDrawArraysEXT(GLenum mode, GLint first, GLsizei count) {
    reportError("glDrawArraysEXT");
  }


  void gles_adp_glGenTexturesEXT(GLsizei n, GLuint *textures) {
    reportError("glGenTexturesEXT");
  };

  void gles_adp_glDeleteTexturesEXT(GLsizei n, const GLuint *textures) {
    reportError("glDeleteTexturesEXT");
  }

  void gles_adp_glBindTextureEXT(GLenum target, GLuint texture) {
    reportError("glBindTextureEXT");
  };

  void
  gles_adp_glPrioritizeTexturesEXT(GLsizei n, const GLuint *textures, const GLclampf *priorities) {
    reportError("glPrioritizeTexturesEXT");
  }


  GLboolean
  gles_adp_glAreTexturesResidentEXT(GLsizei n, const GLuint *textures, GLboolean *residences) {
    reportError("glAreTexturesResidentEXT");
  }


  GLboolean gles_adp_glIsTextureEXT(GLuint texture) {
    reportError("glIsTextureEXT");
  };


  void gles_adp_glTexImage3DEXT(GLenum target, GLint level, GLenum internalFormat, GLsizei width,
                                GLsizei height, GLsizei depth, GLint border, GLenum format,
                                GLenum type, const GLvoid *pixels) {
    reportError("glTexImage3DEXT");
  }


  void
  gles_adp_glTexSubImage3DEXT(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                              GLint zoffset,
                              GLsizei width, GLsizei height, GLsizei depth, GLenum format,
                              GLenum type, const GLvoid *pixels) {
    reportError("glTexSubImage3DEXT");
  }


  void gles_adp_glCopyTexSubImage3DEXT(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                                       GLint zoffset, GLint x, GLint y, GLsizei width,
                                       GLsizei height) {
    reportError("glCopyTexSubImage3DEXT");
  }


  void gles_adp_glColorTableEXT(GLenum target, GLenum internalformat, GLsizei width, GLenum format,
                                GLenum type, const GLvoid *table) {
    reportError("glColorTableEXT");
  }


  void
  gles_adp_glColorSubTableEXT(GLenum target, GLsizei start, GLsizei count, GLenum format,
                              GLenum type,
                              const GLvoid *data) {
    reportError("glColorSubTableEXT");
  }


  void gles_adp_glGetColorTableEXT(GLenum target, GLenum format, GLenum type, GLvoid *table) {
    reportError("glGetColorTableEXT");
  }

  void gles_adp_glGetColorTableParameterfvEXT(GLenum target, GLenum pname, GLfloat *params) {
    reportError("glGetColorTableParameterfvEXT");
  }


  void gles_adp_glGetColorTableParameterivEXT(GLenum target, GLenum pname, GLint *params) {
    reportError("glGetColorTableParameterivEXT");
  }


  void gles_adp_glMultiTexCoord1dSGIS(GLenum target, GLdouble s) {
    reportError("glMultiTexCoord1dSGIS");
  };

  void gles_adp_glMultiTexCoord1dvSGIS(GLenum target, const GLdouble *v) {
    reportError("glMultiTexCoord1dvSGIS");
  };

  void gles_adp_glMultiTexCoord1fSGIS(GLenum target, GLfloat s) {
    reportError("glMultiTexCoord1fSGIS");
  };

  void gles_adp_glMultiTexCoord1fvSGIS(GLenum target, const GLfloat *v) {
    reportError("glMultiTexCoord1fvSGIS");
  };

  void gles_adp_glMultiTexCoord1iSGIS(GLenum target, GLint s) {
    reportError("glMultiTexCoord1iSGIS");
  };

  void gles_adp_glMultiTexCoord1ivSGIS(GLenum target, const GLint *v) {
    reportError("glMultiTexCoord1ivSGIS");
  };

  void gles_adp_glMultiTexCoord1sSGIS(GLenum target, GLshort s) {
    reportError("glMultiTexCoord1sSGIS");
  };

  void gles_adp_glMultiTexCoord1svSGIS(GLenum target, const GLshort *v) {
    reportError("glMultiTexCoord1svSGIS");
  };

  void gles_adp_glMultiTexCoord2dSGIS(GLenum target, GLdouble s, GLdouble t) {
    reportError("glMultiTexCoord2dSGIS");
  };

  void gles_adp_glMultiTexCoord2dvSGIS(GLenum target, const GLdouble *v) {
    reportError("glMultiTexCoord2dvSGIS");
  };

  void gles_adp_glMultiTexCoord2fSGIS(GLenum target, GLfloat s, GLfloat t) {
    reportError("glMultiTexCoord2fSGIS");
  };

  void gles_adp_glMultiTexCoord2fvSGIS(GLenum target, const GLfloat *v) {
    reportError("glMultiTexCoord2fvSGIS");
  };

  void gles_adp_glMultiTexCoord2iSGIS(GLenum target, GLint s, GLint t) {
    reportError("glMultiTexCoord2iSGIS");
  };

  void gles_adp_glMultiTexCoord2ivSGIS(GLenum target, const GLint *v) {
    reportError("glMultiTexCoord2ivSGIS");
  };

  void gles_adp_glMultiTexCoord2sSGIS(GLenum target, GLshort s, GLshort t) {
    reportError("glMultiTexCoord2sSGIS");
  };

  void gles_adp_glMultiTexCoord2svSGIS(GLenum target, const GLshort *v) {
    reportError("glMultiTexCoord2svSGIS");
  };

  void gles_adp_glMultiTexCoord3dSGIS(GLenum target, GLdouble s, GLdouble t, GLdouble r) {
    reportError("glMultiTexCoord3dSGIS");
  };

  void gles_adp_glMultiTexCoord3dvSGIS(GLenum target, const GLdouble *v) {
    reportError("glMultiTexCoord3dvSGIS");
  };

  void gles_adp_glMultiTexCoord3fSGIS(GLenum target, GLfloat s, GLfloat t, GLfloat r) {
    reportError("glMultiTexCoord3fSGIS");
  };

  void gles_adp_glMultiTexCoord3fvSGIS(GLenum target, const GLfloat *v) {
    reportError("glMultiTexCoord3fvSGIS");
  };

  void gles_adp_glMultiTexCoord3iSGIS(GLenum target, GLint s, GLint t, GLint r) {
    reportError("glMultiTexCoord3iSGIS");
  };

  void gles_adp_glMultiTexCoord3ivSGIS(GLenum target, const GLint *v) {
    reportError("glMultiTexCoord3ivSGIS");
  };

  void gles_adp_glMultiTexCoord3sSGIS(GLenum target, GLshort s, GLshort t, GLshort r) {
    reportError("glMultiTexCoord3sSGIS");
  };

  void gles_adp_glMultiTexCoord3svSGIS(GLenum target, const GLshort *v) {
    reportError("glMultiTexCoord3svSGIS");
  };

  void
  gles_adp_glMultiTexCoord4dSGIS(GLenum target, GLdouble s, GLdouble t, GLdouble r, GLdouble q) {
    reportError("glMultiTexCoord4dSGIS");
  };

  void gles_adp_glMultiTexCoord4dvSGIS(GLenum target, const GLdouble *v) {
    reportError("glMultiTexCoord4dvSGIS");
  };

  void gles_adp_glMultiTexCoord4fSGIS(GLenum target, GLfloat s, GLfloat t, GLfloat r, GLfloat q) {
    reportError("glMultiTexCoord4fSGIS");
  };

  void gles_adp_glMultiTexCoord4fvSGIS(GLenum target, const GLfloat *v) {
    reportError("glMultiTexCoord4fvSGIS");
  };

  void gles_adp_glMultiTexCoord4iSGIS(GLenum target, GLint s, GLint t, GLint r, GLint q) {
    reportError("glMultiTexCoord4iSGIS");
  };

  void gles_adp_glMultiTexCoord4ivSGIS(GLenum target, const GLint *v) {
    reportError("glMultiTexCoord4ivSGIS");
  };

  void gles_adp_glMultiTexCoord4sSGIS(GLenum target, GLshort s, GLshort t, GLshort r, GLshort q) {
    reportError("glMultiTexCoord4sSGIS");
  };

  void gles_adp_glMultiTexCoord4svSGIS(GLenum target, const GLshort *v) {
    reportError("glMultiTexCoord4svSGIS");
  };

  void gles_adp_glMultiTexCoordPointerSGIS(GLenum target, GLint size, GLenum type, GLsizei stride,
                                           const GLvoid *pointer) {
    reportError("glMultiTexCoordPointerSGIS");
  };

  void gles_adp_glSelectTextureSGIS(GLenum target) {
    reportError("glSelectTextureSGIS");
  };

  void gles_adp_glSelectTextureCoordSetSGIS(GLenum target) {
    reportError("glSelectTextureCoordSetSGIS");
  };


  void gles_adp_glMultiTexCoord1dEXT(GLenum target, GLdouble s) {
    reportError("glMultiTexCoord1dEXT");
  };

  void gles_adp_glMultiTexCoord1dvEXT(GLenum target, const GLdouble *v) {
    reportError("glMultiTexCoord1dvEXT");
  };

  void gles_adp_glMultiTexCoord1fEXT(GLenum target, GLfloat s) {
    reportError("glMultiTexCoord1fEXT");
  };

  void gles_adp_glMultiTexCoord1fvEXT(GLenum target, const GLfloat *v) {
    reportError("glMultiTexCoord1fvEXT");
  };

  void gles_adp_glMultiTexCoord1iEXT(GLenum target, GLint s) {
    reportError("glMultiTexCoord1iEXT");
  };

  void gles_adp_glMultiTexCoord1ivEXT(GLenum target, const GLint *v) {
    reportError("glMultiTexCoord1ivEXT");
  };

  void gles_adp_glMultiTexCoord1sEXT(GLenum target, GLshort s) {
    reportError("glMultiTexCoord1sEXT");
  };

  void gles_adp_glMultiTexCoord1svEXT(GLenum target, const GLshort *v) {
    reportError("glMultiTexCoord1svEXT");
  };

  void gles_adp_glMultiTexCoord2dEXT(GLenum target, GLdouble s, GLdouble t) {
    reportError("glMultiTexCoord2dEXT");
  };

  void gles_adp_glMultiTexCoord2dvEXT(GLenum target, const GLdouble *v) {
    reportError("glMultiTexCoord2dvEXT");
  };

  void gles_adp_glMultiTexCoord2fEXT(GLenum target, GLfloat s, GLfloat t) {
    reportError("glMultiTexCoord2fEXT");
  };

  void gles_adp_glMultiTexCoord2fvEXT(GLenum target, const GLfloat *v) {
    reportError("glMultiTexCoord2fvEXT");
  };

  void gles_adp_glMultiTexCoord2iEXT(GLenum target, GLint s, GLint t) {
    reportError("glMultiTexCoord2iEXT");
  };

  void gles_adp_glMultiTexCoord2ivEXT(GLenum target, const GLint *v) {
    reportError("glMultiTexCoord2ivEXT");
  };

  void gles_adp_glMultiTexCoord2sEXT(GLenum target, GLshort s, GLshort t) {
    reportError("glMultiTexCoord2sEXT");
  };

  void gles_adp_glMultiTexCoord2svEXT(GLenum target, const GLshort *v) {
    reportError("glMultiTexCoord2svEXT");
  };

  void gles_adp_glMultiTexCoord3dEXT(GLenum target, GLdouble s, GLdouble t, GLdouble r) {
    reportError("glMultiTexCoord3dEXT");
  };

  void gles_adp_glMultiTexCoord3dvEXT(GLenum target, const GLdouble *v) {
    reportError("glMultiTexCoord3dvEXT");
  };

  void gles_adp_glMultiTexCoord3fEXT(GLenum target, GLfloat s, GLfloat t, GLfloat r) {
    reportError("glMultiTexCoord3fEXT");
  };

  void gles_adp_glMultiTexCoord3fvEXT(GLenum target, const GLfloat *v) {
    reportError("glMultiTexCoord3fvEXT");
  };

  void gles_adp_glMultiTexCoord3iEXT(GLenum target, GLint s, GLint t, GLint r) {
    reportError("glMultiTexCoord3iEXT");
  };

  void gles_adp_glMultiTexCoord3ivEXT(GLenum target, const GLint *v) {
    reportError("glMultiTexCoord3ivEXT");
  };

  void gles_adp_glMultiTexCoord3sEXT(GLenum target, GLshort s, GLshort t, GLshort r) {
    reportError("glMultiTexCoord3sEXT");
  };

  void gles_adp_glMultiTexCoord3svEXT(GLenum target, const GLshort *v) {
    reportError("glMultiTexCoord3svEXT");
  };

  void
  gles_adp_glMultiTexCoord4dEXT(GLenum target, GLdouble s, GLdouble t, GLdouble r, GLdouble q) {
    reportError("glMultiTexCoord4dEXT");
  };

  void gles_adp_glMultiTexCoord4dvEXT(GLenum target, const GLdouble *v) {
    reportError("glMultiTexCoord4dvEXT");
  };

  void gles_adp_glMultiTexCoord4fEXT(GLenum target, GLfloat s, GLfloat t, GLfloat r, GLfloat q) {
    reportError("glMultiTexCoord4fEXT");
  };

  void gles_adp_glMultiTexCoord4fvEXT(GLenum target, const GLfloat *v) {
    reportError("glMultiTexCoord4fvEXT");
  };

  void gles_adp_glMultiTexCoord4iEXT(GLenum target, GLint s, GLint t, GLint r, GLint q) {
    reportError("glMultiTexCoord4iEXT");
  };

  void gles_adp_glMultiTexCoord4ivEXT(GLenum target, const GLint *v) {
    reportError("glMultiTexCoord4ivEXT");
  };

  void gles_adp_glMultiTexCoord4sEXT(GLenum target, GLshort s, GLshort t, GLshort r, GLshort q) {
    reportError("glMultiTexCoord4sEXT");
  };

  void gles_adp_glMultiTexCoord4svEXT(GLenum target, const GLshort *v) {
    reportError("glMultiTexCoord4svEXT");
  };

  void gles_adp_glInterleavedTextureCoordSetsEXT(GLint factor) {
    reportError("glInterleavedTextureCoordSetsEXT");
  };

  void gles_adp_glSelectTextureEXT(GLenum target) {
    reportError("glSelectTextureEXT");
  };

  void gles_adp_glSelectTextureCoordSetEXT(GLenum target) {
    reportError("glSelectTextureCoordSetEXT");
  };

  void gles_adp_glSelectTextureTransformEXT(GLenum target) {
    reportError("glSelectTextureTransformEXT");
  };


  void gles_adp_glPointParameterfEXT(GLenum pname, GLfloat param) {
    reportError("glPointParameterfEXT");
  };

  void gles_adp_glPointParameterfvEXT(GLenum pname, const GLfloat *params) {
    reportError("glPointParameterfvEXT");
  }


  void
  gles_adp_glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
                               const GLvoid *indices) {
    reportError("glDrawRangeElements");
  }

  void gles_adp_glTexImage3D(GLenum target, GLint level, GLenum internalFormat, GLsizei width,
                             GLsizei height, GLsizei depth, GLint border, GLenum format,
                             GLenum type,
                             const GLvoid *pixels) {
    reportError("glTexImage3D");
  }


  void
  gles_adp_glTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset,
                           GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type,
                           const GLvoid *pixels) {
    reportError("glTexSubImage3D");
  }


  void gles_adp_glCopyTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                                    GLint zoffset, GLint x, GLint y, GLsizei width,
                                    GLsizei height) {
    reportError("glCopyTexSubImage3D");
  }

}
             