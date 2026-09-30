// syntax-check stub (replaces HPL1Engine/include/GL/GLee.h, which needs real gl.h/glx.h)
#pragma once
#define __GLEE_H__
#include <GL/gl.h>
extern "C" {
// GLee exposes GL 1.2+/extension entry points as function pointers; declare the
// ones the engine headers reference as variadic functions so call sites parse.
#define GLEESTUB(ret, name) ret name(...);
GLEESTUB(void, glActiveTexture) GLEESTUB(void, glClientActiveTexture) GLEESTUB(void, glMultiTexCoord2f) GLEESTUB(void, glBindBuffer) GLEESTUB(void, glGenBuffers) GLEESTUB(void, glDeleteBuffers) GLEESTUB(void, glBufferData) GLEESTUB(void, glBufferSubData)
GLEESTUB(void*, glMapBuffer) GLEESTUB(GLboolean, glUnmapBuffer) GLEESTUB(void, glGenerateMipmapEXT) GLEESTUB(void, glBindFramebufferEXT) GLEESTUB(void, glBindRenderbufferEXT) GLEESTUB(void, glGenFramebuffersEXT) GLEESTUB(void, glGenRenderbuffersEXT)
GLEESTUB(void, glDeleteFramebuffersEXT) GLEESTUB(void, glDeleteRenderbuffersEXT) GLEESTUB(void, glFramebufferTexture2DEXT) GLEESTUB(void, glFramebufferRenderbufferEXT) GLEESTUB(void, glRenderbufferStorageEXT) GLEESTUB(GLenum, glCheckFramebufferStatusEXT)
GLEESTUB(void, glBlendFuncSeparateEXT) GLEESTUB(void, glTexImage3D) GLEESTUB(void, glCompressedTexImage2DARB) GLEESTUB(void, glStencilOpSeparateATI) GLEESTUB(void, glStencilFuncSeparateATI) GLEESTUB(void, glActiveStencilFaceEXT) GLEESTUB(void, glBlendEquation)
GLEESTUB(void, glProgramStringARB) GLEESTUB(void, glBindProgramARB) GLEESTUB(void, glGenProgramsARB) GLEESTUB(void, glDeleteProgramsARB)
#undef GLEESTUB
GLboolean GLeeInit(void);
const char* GLeeGetErrorString(void);
GLboolean GLeeEnabled(GLboolean* extension);
extern GLboolean _GLEE_ARB_multitexture, _GLEE_ARB_vertex_buffer_object, _GLEE_EXT_framebuffer_object, _GLEE_ARB_texture_non_power_of_two, _GLEE_EXT_texture3D, _GLEE_ARB_texture_compression, _GLEE_ARB_fragment_program, _GLEE_ARB_vertex_program, _GLEE_EXT_stencil_two_side, _GLEE_ATI_separate_stencil, _GLEE_EXT_blend_func_separate, _GLEE_EXT_texture_env_combine, _GLEE_ARB_texture_env_combine, _GLEE_EXT_texture_filter_anisotropic, _GLEE_ARB_texture_rectangle, _GLEE_EXT_texture_rectangle, _GLEE_NV_texture_rectangle, _GLEE_ARB_multisample, _GLEE_EXT_packed_depth_stencil, _GLEE_ARB_texture_float, _GLEE_ARB_texture_cube_map, _GLEE_EXT_stencil_wrap, _GLEE_SGIS_generate_mipmap, _GLEE_EXT_bgra;
#define GLEE_ARB_multitexture _GLEE_ARB_multitexture
#define GLEE_ARB_vertex_buffer_object _GLEE_ARB_vertex_buffer_object
#define GLEE_EXT_framebuffer_object _GLEE_EXT_framebuffer_object
#define GLEE_ARB_texture_non_power_of_two _GLEE_ARB_texture_non_power_of_two
#define GLEE_EXT_texture3D _GLEE_EXT_texture3D
#define GLEE_ARB_texture_compression _GLEE_ARB_texture_compression
#define GLEE_ARB_fragment_program _GLEE_ARB_fragment_program
#define GLEE_ARB_vertex_program _GLEE_ARB_vertex_program
#define GLEE_EXT_stencil_two_side _GLEE_EXT_stencil_two_side
#define GLEE_ATI_separate_stencil _GLEE_ATI_separate_stencil
#define GLEE_EXT_blend_func_separate _GLEE_EXT_blend_func_separate
#define GLEE_EXT_texture_env_combine _GLEE_EXT_texture_env_combine
#define GLEE_ARB_texture_env_combine _GLEE_ARB_texture_env_combine
#define GLEE_EXT_texture_filter_anisotropic _GLEE_EXT_texture_filter_anisotropic
#define GLEE_ARB_texture_rectangle _GLEE_ARB_texture_rectangle
#define GLEE_EXT_texture_rectangle _GLEE_EXT_texture_rectangle
#define GLEE_NV_texture_rectangle _GLEE_NV_texture_rectangle
#define GLEE_ARB_multisample _GLEE_ARB_multisample
#define GLEE_EXT_packed_depth_stencil _GLEE_EXT_packed_depth_stencil
#define GLEE_ARB_texture_float _GLEE_ARB_texture_float
#define GLEE_ARB_texture_cube_map _GLEE_ARB_texture_cube_map
#define GLEE_EXT_stencil_wrap _GLEE_EXT_stencil_wrap
#define GLEE_SGIS_generate_mipmap _GLEE_SGIS_generate_mipmap
#define GLEE_EXT_bgra _GLEE_EXT_bgra
}
