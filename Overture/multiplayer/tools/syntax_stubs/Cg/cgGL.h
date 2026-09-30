// syntax-check stub
#pragma once
#include <Cg/cg.h>
typedef int CGGLenum;
enum { CG_GL_MATRIX_IDENTITY = 0, CG_GL_MATRIX_TRANSPOSE = 1, CG_GL_MATRIX_INVERSE = 2, CG_GL_MATRIX_INVERSE_TRANSPOSE = 3, CG_GL_MODELVIEW_MATRIX = 4, CG_GL_PROJECTION_MATRIX = 5, CG_GL_TEXTURE_MATRIX = 6, CG_GL_MODELVIEW_PROJECTION_MATRIX = 7, CG_GL_VERTEX = 8, CG_GL_FRAGMENT = 9 };
extern "C" {
CGbool cgGLIsProfileSupported(CGprofile); void cgGLEnableProfile(CGprofile); void cgGLDisableProfile(CGprofile); CGprofile cgGLGetLatestProfile(CGGLenum); void cgGLSetOptimalOptions(CGprofile); void cgGLLoadProgram(CGprogram); CGbool cgGLIsProgramLoaded(CGprogram); void cgGLBindProgram(CGprogram); void cgGLUnbindProgram(CGprofile);
void cgGLSetParameter1f(CGparameter, float); void cgGLSetParameter2f(CGparameter, float, float); void cgGLSetParameter3f(CGparameter, float, float, float); void cgGLSetParameter4f(CGparameter, float, float, float, float); void cgGLSetParameter4fv(CGparameter, const float*); void cgGLSetStateMatrixParameter(CGparameter, CGGLenum, CGGLenum); void cgGLSetMatrixParameterfr(CGparameter, const float*); void cgGLSetMatrixParameterfc(CGparameter, const float*);
void cgGLSetTextureParameter(CGparameter, unsigned int); void cgGLEnableTextureParameter(CGparameter); void cgGLDisableTextureParameter(CGparameter); void cgGLSetManageTextureParameters(CGcontext, CGbool);
}
