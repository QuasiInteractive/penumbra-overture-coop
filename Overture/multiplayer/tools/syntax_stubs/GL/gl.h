// syntax-check stub
#pragma once
typedef unsigned int GLenum; typedef unsigned char GLboolean; typedef unsigned int GLbitfield; typedef signed char GLbyte; typedef short GLshort; typedef int GLint; typedef int GLsizei;
typedef unsigned char GLubyte; typedef unsigned short GLushort; typedef unsigned int GLuint; typedef float GLfloat; typedef float GLclampf; typedef double GLdouble; typedef double GLclampd; typedef void GLvoid;
typedef char GLchar; typedef long GLintptr; typedef long GLsizeiptr; typedef char GLcharARB; typedef unsigned int GLhandleARB; typedef long GLintptrARB; typedef long GLsizeiptrARB;
#define GL_FALSE 0
#define GL_TRUE 1
#define GL_STUB_CONST(n) 0x8000
#define GL_TEXTURE_2D 0x0DE1
#define GL_RGBA 0x1908
#define GL_RGB 0x1907
#define GL_UNSIGNED_BYTE 0x1401
#define GL_FLOAT 0x1406
#define GL_TRIANGLES 0x0004
#define GL_TRIANGLE_STRIP 0x0005
#define GL_QUADS 0x0007
#define GL_LINES 0x0001
#define GL_POINTS 0x0000
#define GL_LINE_STRIP 0x0003
#define GL_UNSIGNED_INT 0x1405
#define GL_UNSIGNED_SHORT 0x1403
#define GL_DEPTH_TEST 0x0B71
#define GL_BLEND 0x0BE2
#define GL_ONE 1
#define GL_ZERO 0
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_MODELVIEW 0x1700
#define GL_PROJECTION 0x1701
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_DEPTH_BUFFER_BIT 0x100
#define GL_STENCIL_BUFFER_BIT 0x400
extern "C" {
void glEnable(GLenum); void glDisable(GLenum); void glBegin(GLenum); void glEnd(void); void glVertex3f(GLfloat,GLfloat,GLfloat); void glColor4f(GLfloat,GLfloat,GLfloat,GLfloat);
void glBindTexture(GLenum,GLuint); void glMatrixMode(GLenum); void glLoadMatrixf(const GLfloat*); void glLoadIdentity(void); void glPushMatrix(void); void glPopMatrix(void);
void glClear(GLbitfield); void glBlendFunc(GLenum,GLenum); void glGetIntegerv(GLenum,GLint*); void glGetFloatv(GLenum,GLfloat*); const GLubyte* glGetString(GLenum); GLenum glGetError(void);
void glTexCoord2f(GLfloat,GLfloat); void glViewport(GLint,GLint,GLsizei,GLsizei); void glGenTextures(GLsizei,GLuint*); void glDeleteTextures(GLsizei,const GLuint*); void glTexImage2D(GLenum,GLint,GLint,GLsizei,GLsizei,GLint,GLenum,GLenum,const GLvoid*);
void glTexParameteri(GLenum,GLenum,GLint); void glReadPixels(GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,GLvoid*); void glFlush(void); void glFinish(void); void glDepthMask(GLboolean); void glDepthFunc(GLenum);
void glLineWidth(GLfloat); void glPointSize(GLfloat); void glColor3f(GLfloat,GLfloat,GLfloat); void glVertex2f(GLfloat,GLfloat); void glNormal3f(GLfloat,GLfloat,GLfloat); void glClearColor(GLclampf,GLclampf,GLclampf,GLclampf);
}
