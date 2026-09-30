// syntax-check stub
#pragma once
typedef struct _CGcontext* CGcontext; typedef struct _CGprogram* CGprogram; typedef struct _CGparameter* CGparameter; typedef struct _CGeffect* CGeffect;
typedef int CGprofile; typedef int CGerror; typedef int CGenum; typedef int CGbool; typedef int CGtype;
typedef void (*CGerrorCallbackFunc)(void);
enum { CG_NO_ERROR = 0, CG_SOURCE = 4112, CG_OBJECT = 4113, CG_PROFILE_UNKNOWN = 6145, CG_PROFILE_VP20 = 6146, CG_PROFILE_FP20 = 6147, CG_PROFILE_VP30 = 6148, CG_PROFILE_FP30 = 6149, CG_PROFILE_ARBVP1 = 6150, CG_PROFILE_ARBFP1 = 7000, CG_PROFILE_VP40 = 7001, CG_PROFILE_FP40 = 6151, CG_PROFILE_GLSLV = 7007, CG_PROFILE_GLSLF = 7008, CG_PROFILE_GLSLC = 7009, CG_TRUE = 1, CG_FALSE = 0 };
extern "C" {
CGcontext cgCreateContext(void); void cgDestroyContext(CGcontext); CGprogram cgCreateProgram(CGcontext, CGenum, const char*, CGprofile, const char*, const char**); CGprogram cgCreateProgramFromFile(CGcontext, CGenum, const char*, CGprofile, const char*, const char**); void cgDestroyProgram(CGprogram);
CGparameter cgGetNamedParameter(CGprogram, const char*); CGerror cgGetError(void); const char* cgGetErrorString(CGerror); const char* cgGetLastListing(CGcontext); void cgSetErrorCallback(CGerrorCallbackFunc); CGbool cgIsProgram(CGprogram); CGbool cgIsParameter(CGparameter); const char* cgGetProgramString(CGprogram, CGenum);
void cgSetParameter1f(CGparameter, float); void cgSetParameter2f(CGparameter, float, float); void cgSetParameter3f(CGparameter, float, float, float); void cgSetParameter4f(CGparameter, float, float, float, float); void cgSetParameter4fv(CGparameter, const float*); void cgSetMatrixParameterfr(CGparameter, const float*); void cgSetMatrixParameterfc(CGparameter, const float*);
}
