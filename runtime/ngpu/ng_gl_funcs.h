// Native GE renderer: the OpenGL 3.3 core functions it calls, loaded through the frontend's get_proc
// (so nothing links against libGL). Include this instead of <GL/gl.h> in the renderer's files.
#pragma once
#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES 1   // only for decltype() below
#endif
#include <GL/gl.h>
#include <GL/glext.h>

#define NG_GL_FUNCS(X) \
	X(glActiveTexture) X(glAttachShader) X(glBindAttribLocation) X(glBindBuffer) X(glBindFramebuffer) \
	X(glBindFragDataLocation) X(glBindRenderbuffer) X(glBindTexture) X(glBindVertexArray) X(glBlendColor) \
	X(glBlendEquationSeparate) X(glBlendFuncSeparate) X(glBlitFramebuffer) X(glBufferData) X(glBufferSubData) \
	X(glCheckFramebufferStatus) X(glClear) X(glClearColor) X(glClearDepth) X(glClearStencil) X(glColorMask) \
	X(glCompileShader) X(glCreateProgram) X(glCreateShader) X(glCullFace) X(glDeleteBuffers) \
	X(glDeleteFramebuffers) X(glDeleteProgram) X(glDeleteRenderbuffers) X(glDeleteShader) X(glDeleteTextures) \
	X(glDeleteVertexArrays) X(glDepthFunc) X(glDepthMask) X(glDepthRange) X(glDisable) \
	X(glDisableVertexAttribArray) X(glDrawArrays) X(glDrawElementsBaseVertex) X(glEnable) \
	X(glEnableVertexAttribArray) X(glFlush) X(glFramebufferRenderbuffer) X(glFramebufferTexture2D) X(glFrontFace) \
	X(glGenBuffers) X(glGenFramebuffers) X(glGenRenderbuffers) X(glGenTextures) X(glGenVertexArrays) \
	X(glGenerateMipmap) X(glGetError) X(glGetFloatv) X(glGetIntegerv) X(glGetProgramInfoLog) X(glGetProgramiv) \
	X(glGetShaderInfoLog) X(glGetShaderiv) X(glGetString) X(glGetUniformLocation) X(glLinkProgram) \
	X(glPixelStorei) X(glReadBuffer) X(glReadPixels) X(glRenderbufferStorage) X(glScissor) X(glShaderSource) \
	X(glTexImage2D) X(glTexParameterf) X(glTexParameteri) X(glTexSubImage2D) X(glUniform1f) X(glUniform1i) \
	X(glUniform2f) X(glUniform3f) X(glUniform3fv) X(glUniform4f) X(glUniform4fv) X(glUniformMatrix4fv) \
	X(glUseProgram) X(glVertexAttribPointer) X(glViewport) X(glDrawBuffer) X(glBindBufferBase) \
	X(glGetUniformBlockIndex) X(glUniformBlockBinding) X(glProvokingVertex)

#define NG_GL_DECL(f) extern decltype(&::f) ng_##f;
NG_GL_FUNCS(NG_GL_DECL)
#undef NG_GL_DECL
#ifndef NG_GL_LOADER_IMPL
// (each gl* name below is the loaded pointer)
#define glActiveTexture ng_glActiveTexture
#define glAttachShader ng_glAttachShader
#define glBindAttribLocation ng_glBindAttribLocation
#define glBindBuffer ng_glBindBuffer
#define glBindFramebuffer ng_glBindFramebuffer
#define glBindFragDataLocation ng_glBindFragDataLocation
#define glBindRenderbuffer ng_glBindRenderbuffer
#define glBindTexture ng_glBindTexture
#define glBindVertexArray ng_glBindVertexArray
#define glBlendColor ng_glBlendColor
#define glBlendEquationSeparate ng_glBlendEquationSeparate
#define glBlendFuncSeparate ng_glBlendFuncSeparate
#define glBlitFramebuffer ng_glBlitFramebuffer
#define glBufferData ng_glBufferData
#define glBufferSubData ng_glBufferSubData
#define glCheckFramebufferStatus ng_glCheckFramebufferStatus
#define glClear ng_glClear
#define glClearColor ng_glClearColor
#define glClearDepth ng_glClearDepth
#define glClearStencil ng_glClearStencil
#define glColorMask ng_glColorMask
#define glCompileShader ng_glCompileShader
#define glCreateProgram ng_glCreateProgram
#define glCreateShader ng_glCreateShader
#define glCullFace ng_glCullFace
#define glDeleteBuffers ng_glDeleteBuffers
#define glDeleteFramebuffers ng_glDeleteFramebuffers
#define glDeleteProgram ng_glDeleteProgram
#define glDeleteRenderbuffers ng_glDeleteRenderbuffers
#define glDeleteShader ng_glDeleteShader
#define glDeleteTextures ng_glDeleteTextures
#define glDeleteVertexArrays ng_glDeleteVertexArrays
#define glDepthFunc ng_glDepthFunc
#define glDepthMask ng_glDepthMask
#define glDepthRange ng_glDepthRange
#define glDisable ng_glDisable
#define glDisableVertexAttribArray ng_glDisableVertexAttribArray
#define glDrawArrays ng_glDrawArrays
#define glDrawElementsBaseVertex ng_glDrawElementsBaseVertex
#define glEnable ng_glEnable
#define glEnableVertexAttribArray ng_glEnableVertexAttribArray
#define glFlush ng_glFlush
#define glFramebufferRenderbuffer ng_glFramebufferRenderbuffer
#define glFramebufferTexture2D ng_glFramebufferTexture2D
#define glFrontFace ng_glFrontFace
#define glGenBuffers ng_glGenBuffers
#define glGenFramebuffers ng_glGenFramebuffers
#define glGenRenderbuffers ng_glGenRenderbuffers
#define glGenTextures ng_glGenTextures
#define glGenVertexArrays ng_glGenVertexArrays
#define glGenerateMipmap ng_glGenerateMipmap
#define glGetError ng_glGetError
#define glGetFloatv ng_glGetFloatv
#define glGetIntegerv ng_glGetIntegerv
#define glGetProgramInfoLog ng_glGetProgramInfoLog
#define glGetProgramiv ng_glGetProgramiv
#define glGetShaderInfoLog ng_glGetShaderInfoLog
#define glGetShaderiv ng_glGetShaderiv
#define glGetString ng_glGetString
#define glGetUniformLocation ng_glGetUniformLocation
#define glLinkProgram ng_glLinkProgram
#define glPixelStorei ng_glPixelStorei
#define glReadBuffer ng_glReadBuffer
#define glReadPixels ng_glReadPixels
#define glRenderbufferStorage ng_glRenderbufferStorage
#define glScissor ng_glScissor
#define glShaderSource ng_glShaderSource
#define glTexImage2D ng_glTexImage2D
#define glTexParameterf ng_glTexParameterf
#define glTexParameteri ng_glTexParameteri
#define glTexSubImage2D ng_glTexSubImage2D
#define glUniform1f ng_glUniform1f
#define glUniform1i ng_glUniform1i
#define glUniform2f ng_glUniform2f
#define glUniform3f ng_glUniform3f
#define glUniform3fv ng_glUniform3fv
#define glUniform4f ng_glUniform4f
#define glUniform4fv ng_glUniform4fv
#define glUniformMatrix4fv ng_glUniformMatrix4fv
#define glUseProgram ng_glUseProgram
#define glVertexAttribPointer ng_glVertexAttribPointer
#define glViewport ng_glViewport
#define glDrawBuffer ng_glDrawBuffer
#define glBindBufferBase ng_glBindBufferBase
#define glGetUniformBlockIndex ng_glGetUniformBlockIndex
#define glUniformBlockBinding ng_glUniformBlockBinding
#define glProvokingVertex ng_glProvokingVertex
#endif

// Returns the number of functions the context doesn't have.
int ng_gl_load(void *(*get_proc)(const char *));
