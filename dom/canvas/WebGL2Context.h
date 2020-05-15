/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef WEBGL2CONTEXT_H_
#define WEBGL2CONTEXT_H_

#include "WebGLContext.h"

namespace mozilla {

class ErrorResult;
class WebGLSampler;
class WebGLSync;
class WebGLTransformFeedback;
class WebGLVertexArrayObject;
namespace dom {
class OwningUnsignedLongOrUint32ArrayOrBoolean;
class OwningWebGLBufferOrLongLong;
}  // namespace dom

class WebGL2Context : public WebGLContext {
 public:
  virtual ~WebGL2Context();

  static bool IsSupported();
  static WebGL2Context* Create();

  virtual bool IsWebGL2() const override { return true; }

  // -------------------------------------------------------------------------
  // IMPLEMENT nsWrapperCache

  virtual JSObject* WrapObject(JSContext* cx,
                               JS::Handle<JSObject*> givenProto) override;

  // -------------------------------------------------------------------------
  // Buffer objects - WebGL2ContextBuffers.cpp

  void CopyBufferSubData(GLenum readTarget, GLenum writeTarget,
                         WebGLintptr readOffset, WebGLintptr writeOffset,
                         WebGLsizeiptr size);

 private:
  template <typename BufferT>
  void GetBufferSubDataT(GLenum target, WebGLintptr offset,
                         const BufferT& data);

 public:
  void GetBufferSubData(GLenum target, WebGLintptr srcByteOffset,
                        const dom::ArrayBufferView& dstData,
                        GLuint dstElemOffset, GLuint dstElemCountOverride);

  // -------------------------------------------------------------------------
  // Framebuffer objects - WebGL2ContextFramebuffers.cpp

  void BlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                       GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
                       GLbitfield mask, GLenum filter);
  void FramebufferTextureLayer(GLenum target, GLenum attachment,
                               WebGLTexture* texture, GLint level, GLint layer);

  virtual JS::Value GetFramebufferAttachmentParameter(JSContext* cx,
                                                      GLenum target,
                                                      GLenum attachment,
                                                      GLenum pname,
                                                      ErrorResult& rv) override;
  // Make the inline version from the superclass visible here.
  using WebGLContext::GetFramebufferAttachmentParameter;

  void InvalidateFramebuffer(GLenum target,
                             const dom::Sequence<GLenum>& attachments,
                             ErrorResult& rv);
  void InvalidateSubFramebuffer(GLenum target,
                                const dom::Sequence<GLenum>& attachments,
                                GLint x, GLint y, GLsizei width, GLsizei height,
                                ErrorResult& rv);
  void ReadBuffer(GLenum mode);

  // -------------------------------------------------------------------------
  // Renderbuffer objects - WebGL2ContextRenderbuffers.cpp

  void GetInternalformatParameter(JSContext*, GLenum target,
                                  GLenum internalformat, GLenum pname,
                                  JS::MutableHandleValue retval,
                                  ErrorResult& rv);
  void RenderbufferStorageMultisample(GLenum target, GLsizei samples,
                                      GLenum internalFormat, GLsizei width,
                                      GLsizei height) {
    const FuncScope funcScope(*this, "renderbufferStorageMultisample");
    RenderbufferStorage_base(target, samples, internalFormat, width, height);
  }

  // -------------------------------------------------------------------------
  // Texture objects - WebGL2ContextTextures.cpp

  void TexStorage2D(GLenum target, GLsizei levels, GLenum internalFormat,
                    GLsizei width, GLsizei height) {
    const FuncScope funcScope(*this, "TexStorage2D");
    const uint8_t funcDims = 2;
    const GLsizei depth = 1;
    TexStorage(funcDims, target, levels, internalFormat, width, height, depth);
  }

  void TexStorage3D(GLenum target, GLsizei levels, GLenum internalFormat,
                    GLsizei width, GLsizei height, GLsizei depth) {
    const FuncScope funcScope(*this, "TexStorage3D");
    const uint8_t funcDims = 3;
    TexStorage(funcDims, target, levels, internalFormat, width, height, depth);
  }

 protected:
  void TexStorage(uint8_t funcDims, GLenum target, GLsizei levels,
                  GLenum internalFormat, GLsizei width, GLsizei height,
                  GLsizei depth);

  ////////////////////////////////////

 public:
  void CompressedTexImage3D(GLenum target, GLint level, GLenum internalFormat,
                            GLsizei width, GLsizei height, GLsizei depth,
                            GLint border, GLsizei imageSize,
                            WebGLintptr offset) {
    const FuncScope funcScope(*this, "compressedTexImage3D");
    const uint8_t funcDims = 3;
    const TexImageSourceAdapter src(&offset, 0, 0);
    CompressedTexImage(funcDims, target, level, internalFormat, width, height,
                       depth, border, src, Some(imageSize));
  }

  template <typename T>
  void CompressedTexImage3D(GLenum target, GLint level, GLenum internalFormat,
                            GLsizei width, GLsizei height, GLsizei depth,
                            GLint border, const T& anySrc,
                            GLuint viewElemOffset = 0,
                            GLuint viewElemLengthOverride = 0) {
    const FuncScope funcScope(*this, "compressedTexImage3D");
    const uint8_t funcDims = 3;
    const TexImageSourceAdapter src(&anySrc, viewElemOffset,
                                    viewElemLengthOverride);
    CompressedTexImage(funcDims, target, level, internalFormat, width, height,
                       depth, border, src, Nothing());
  }

  void CompressedTexSubImage3D(GLenum target, GLint level, GLint xOffset,
                               GLint yOffset, GLint zOffset, GLsizei width,
                               GLsizei height, GLsizei depth,
                               GLenum unpackFormat, GLsizei imageSize,
                               WebGLintptr offset) {
    const FuncScope funcScope(*this, "compressedTexSubImage3D");
    const uint8_t funcDims = 3;
    const TexImageSourceAdapter src(&offset, 0, 0);
    CompressedTexSubImage(funcDims, target, level, xOffset, yOffset, zOffset,
                          width, height, depth, unpackFormat, src,
                          Some(imageSize));
  }

  template <typename T>
  void CompressedTexSubImage3D(GLenum target, GLint level, GLint xOffset,
                               GLint yOffset, GLint zOffset, GLsizei width,
                               GLsizei height, GLsizei depth,
                               GLenum unpackFormat, const T& anySrc,
                               GLuint viewElemOffset = 0,
                               GLuint viewElemLengthOverride = 0) {
    const FuncScope funcScope(*this, "compressedTexSubImage3D");
    const uint8_t funcDims = 3;
    const TexImageSourceAdapter src(&anySrc, viewElemOffset,
                                    viewElemLengthOverride);
    CompressedTexSubImage(funcDims, target, level, xOffset, yOffset, zOffset,
                          width, height, depth, unpackFormat, src, Nothing());
  }

  ////////////////////////////////////

  void CopyTexSubImage3D(GLenum target, GLint level, GLint xOffset,
                         GLint yOffset, GLint zOffset, GLint x, GLint y,
                         GLsizei width, GLsizei height) {
    const FuncScope funcScope(*this, "copyTexSubImage3D");
    const uint8_t funcDims = 3;
    CopyTexSubImage(funcDims, target, level, xOffset, yOffset, zOffset, x, y,
                    width, height);
  }

  ////////////////////////////////////

  template <typename T>
  void TexImage3D(GLenum target, GLint level, GLenum internalFormat,
                  GLsizei width, GLsizei height, GLsizei depth, GLint border,
                  GLenum unpackFormat, GLenum unpackType, const T& anySrc,
                  ErrorResult& out_error) {
    const TexImageSourceAdapter src(&anySrc, &out_error);
    TexImage3D(target, level, internalFormat, width, height, depth, border,
               unpackFormat, unpackType, src);
  }

  void TexImage3D(GLenum target, GLint level, GLenum internalFormat,
                  GLsizei width, GLsizei height, GLsizei depth, GLint border,
                  GLenum unpackFormat, GLenum unpackType,
                  const dom::ArrayBufferView& view, GLuint viewElemOffset,
                  ErrorResult&) {
    const TexImageSourceAdapter src(&view, viewElemOffset);
    TexImage3D(target, level, internalFormat, width, height, depth, border,
               unpackFormat, unpackType, src);
  }

 protected:
  void TexImage3D(GLenum target, GLint level, GLenum internalFormat,
                  GLsizei width, GLsizei height, GLsizei depth, GLint border,
                  GLenum unpackFormat, GLenum unpackType,
                  const TexImageSource& src) {
    const FuncScope funcScope(*this, "texImage3D");
    const uint8_t funcDims = 3;
    TexImage(funcDims, target, level, internalFormat, width, height, depth,
             border, unpackFormat, unpackType, src);
  }

  ////////////////////////////////////

 public:
  template <typename T>
  void TexSubImage3D(GLenum target, GLint level, GLint xOffset, GLint yOffset,
                     GLint zOffset, GLsizei width, GLsizei height,
                     GLsizei depth, GLenum unpackFormat, GLenum unpackType,
                     const T& anySrc, ErrorResult& out_error) {
    const TexImageSourceAdapter src(&anySrc, &out_error);
    TexSubImage3D(target, level, xOffset, yOffset, zOffset, width, height,
                  depth, unpackFormat, unpackType, src);
  }

  void TexSubImage3D(GLenum target, GLint level, GLint xOffset, GLint yOffset,
                     GLint zOffset, GLsizei width, GLsizei height,
                     GLsizei depth, GLenum unpackFormat, GLenum unpackType,
                     const dom::Nullable<dom::ArrayBufferView>& maybeSrcView,
                     GLuint srcElemOffset, ErrorResult&) {
    const FuncScope funcScope(*this, "texSubImage3D");
    if (IsContextLost()) return;

    if (!ValidateNonNull("src", maybeSrcView)) return;
    const auto& srcView = maybeSrcView.Value();

    const TexImageSourceAdapter src(&srcView, srcElemOffset);
    TexSubImage3D(target, level, xOffset, yOffset, zOffset, width, height,
                  depth, unpackFormat, unpackType, src);
  }

 protected:
  void TexSubImage3D(GLenum target, GLint level, GLint xOffset, GLint yOffset,
                     GLint zOffset, GLsizei width, GLsizei height,
                     GLsizei depth, GLenum unpackFormat, GLenum unpackType,
                     const TexImageSource& src) {
    const FuncScope funcScope(*this, "texSubImage3D");
    const uint8_t funcDims = 3;
    TexSubImage(funcDims, target, level, xOffset, yOffset, zOffset, width,
                height, depth, unpackFormat, unpackType, src);
  }

 public:
  // -------------------------------------------------------------------------
  // Programs and shaders - WebGL2ContextPrograms.cpp
  GLint GetFragDataLocation(const WebGLProgram& program, const nsAString& name);

  // -------------------------------------------------------------------------
  // Uniforms and attributes - WebGL2ContextUniforms.cpp

  void VertexAttribIPointer(GLuint index, GLint size, GLenum type,
                            GLsizei stride, WebGLintptr byteOffset) {
    const FuncScope funcScope(*this, "vertexAttribIPointer");
    const bool isFuncInt = true;
    const bool normalized = false;
    VertexAttribAnyPointer(isFuncInt, index, size, type, normalized, stride,
                           byteOffset);
  }

  ////////////////

  // GL 3.0 & ES 3.0
  void VertexAttribI4i(GLuint index, GLint x, GLint y, GLint z, GLint w);
  void VertexAttribI4ui(GLuint index, GLuint x, GLuint y, GLuint z, GLuint w);

  void VertexAttribI4iv(GLuint index, const Int32ListU& list) {
    const FuncScope funcScope(*this, "VertexAttribI4iv");
    const auto& arr = Int32Arr::From(list);
    if (!ValidateAttribArraySetter(4, arr.elemCount)) return;

    const auto& itr = arr.elemBytes;
    VertexAttribI4i(index, itr[0], itr[1], itr[2], itr[3]);
  }

  void VertexAttribI4uiv(GLuint index, const Uint32ListU& list) {
    const FuncScope funcScope(*this, "vertexAttribI4uiv");
    const auto& arr = Uint32Arr::From(list);
    if (!ValidateAttribArraySetter(4, arr.elemCount)) return;

    const auto& itr = arr.elemBytes;
    VertexAttribI4ui(index, itr[0], itr[1], itr[2], itr[3]);
  }

  // -------------------------------------------------------------------------
  // Writing to the drawing buffer

  /* Implemented in WebGLContext
  void VertexAttribDivisor(GLuint index, GLuint divisor);
  void DrawArraysInstanced(GLenum mode, GLint first, GLsizei count,
                           GLsizei instanceCount);
  void DrawElementsInstanced(GLenum mode, GLsizei count, GLenum type,
                             WebGLintptr offset, GLsizei instanceCount);
  */

  void DrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count,
                         GLenum type, WebGLintptr byteOffset) {
    const FuncScope funcScope(*this, "drawRangeElements");
    if (IsContextLost()) return;

    if (end < start) {
      ErrorInvalidValue("end must be >= start.");
      return;
    }

    DrawElements(mode, count, type, byteOffset);
  }

  // ------------------------------------------------------------------------
  // Multiple Render Targets - WebGL2ContextMRTs.cpp
  /* Implemented in WebGLContext
  void DrawBuffers(const dom::Sequence<GLenum>& buffers);
  */

 private:
  bool ValidateClearBuffer(GLenum buffer, GLint drawBuffer,
                           size_t availElemCount, GLuint elemOffset,
                           GLenum funcType);

  void ClearBufferfv(GLenum buffer, GLint drawBuffer, const Float32Arr& src,
                     GLuint srcElemOffset);
  void ClearBufferiv(GLenum buffer, GLint drawBuffer, const Int32Arr& src,
                     GLuint srcElemOffset);
  void ClearBufferuiv(GLenum buffer, GLint drawBuffer, const Uint32Arr& src,
                      GLuint srcElemOffset);

 public:
  void ClearBufferfv(GLenum buffer, GLint drawBuffer, const Float32ListU& list,
                     GLuint srcElemOffset) {
    ClearBufferfv(buffer, drawBuffer, Float32Arr::From(list), srcElemOffset);
  }
  void ClearBufferiv(GLenum buffer, GLint drawBuffer, const Int32ListU& list,
                     GLuint srcElemOffset) {
    ClearBufferiv(buffer, drawBuffer, Int32Arr::From(list), srcElemOffset);
  }
  void ClearBufferuiv(GLenum buffer, GLint drawBuffer, const Uint32ListU& list,
                      GLuint srcElemOffset) {
    ClearBufferuiv(buffer, drawBuffer, Uint32Arr::From(list), srcElemOffset);
  }

  void ClearBufferfi(GLenum buffer, GLint drawBuffer, GLfloat depth,
                     GLint stencil);

  // -------------------------------------------------------------------------
  // Sampler Objects - WebGL2ContextSamplers.cpp

  already_AddRefed<WebGLSampler> CreateSampler();
  void DeleteSampler(WebGLSampler* sampler);
  bool IsSampler(const WebGLSampler* sampler);
  void BindSampler(GLuint unit, WebGLSampler* sampler);
  void SamplerParameteri(WebGLSampler& sampler, GLenum pname, GLint param);
  void SamplerParameterf(WebGLSampler& sampler, GLenum pname, GLfloat param);
  void GetSamplerParameter(JSContext*, const WebGLSampler& sampler,
                           GLenum pname, JS::MutableHandleValue retval);

  // -------------------------------------------------------------------------
  // Sync objects - WebGL2ContextSync.cpp

  const GLuint64 kMaxClientWaitSyncTimeoutNS =
      1000 * 1000 * 1000;  // 1000ms in ns.

  already_AddRefed<WebGLSync> FenceSync(GLenum condition, GLbitfield flags);
  bool IsSync(const WebGLSync* sync);
  void DeleteSync(WebGLSync* sync);
  GLenum ClientWaitSync(const WebGLSync& sync, GLbitfield flags,
                        GLuint64 timeout);
  void WaitSync(const WebGLSync& sync, GLbitfield flags, GLint64 timeout);
  void GetSyncParameter(JSContext*, const WebGLSync& sync, GLenum pname,
                        JS::MutableHandleValue retval);

  // -------------------------------------------------------------------------
  // Transform Feedback - WebGL2ContextTransformFeedback.cpp

  already_AddRefed<WebGLTransformFeedback> CreateTransformFeedback();
  void DeleteTransformFeedback(WebGLTransformFeedback* tf);
  bool IsTransformFeedback(const WebGLTransformFeedback* tf);
  void BindTransformFeedback(GLenum target, WebGLTransformFeedback* tf);
  void BeginTransformFeedback(GLenum primitiveMode);
  void EndTransformFeedback();
  void PauseTransformFeedback();
  void ResumeTransformFeedback();
  void TransformFeedbackVaryings(WebGLProgram& program,
                                 const dom::Sequence<nsString>& varyings,
                                 GLenum bufferMode);
  already_AddRefed<WebGLActiveInfo> GetTransformFeedbackVarying(
      const WebGLProgram& program, GLuint index);

  // -------------------------------------------------------------------------
  // Uniform Buffer Objects and Transform Feedback Buffers -
  // WebGL2ContextUniforms.cpp
  // TODO(djg): Implemented in WebGLContext
  /*
      void BindBufferBase(GLenum target, GLuint index, WebGLBuffer* buffer);
      void BindBufferRange(GLenum target, GLuint index, WebGLBuffer* buffer,
                           WebGLintptr offset, WebGLsizeiptr size);
  */
  virtual JS::Value GetParameter(JSContext* cx, GLenum pname,
                                 ErrorResult& rv) override;
  // Make the inline version from the superclass visible here.
  using WebGLContext::GetParameter;
  void GetIndexedParameter(JSContext* cx, GLenum target, GLuint index,
                           JS::MutableHandleValue retval, ErrorResult& rv);
  void GetUniformIndices(const WebGLProgram& program,
                         const dom::Sequence<nsString>& uniformNames,
                         dom::Nullable<nsTArray<GLuint> >& retval);
  void GetActiveUniforms(JSContext* cx, const WebGLProgram& program,
                         const dom::Sequence<GLuint>& uniformIndices,
                         GLenum pname, JS::MutableHandleValue retval);

  GLuint GetUniformBlockIndex(const WebGLProgram& program,
                              const nsAString& uniformBlockName);
  void GetActiveUniformBlockParameter(JSContext*, const WebGLProgram& program,
                                      GLuint uniformBlockIndex, GLenum pname,
                                      JS::MutableHandleValue retval,
                                      ErrorResult& rv);
  void GetActiveUniformBlockName(const WebGLProgram& program,
                                 GLuint uniformBlockIndex, nsAString& retval);
  void UniformBlockBinding(WebGLProgram& program, GLuint uniformBlockIndex,
                           GLuint uniformBlockBinding);

  // -------------------------------------------------------------------------
  // Vertex Array Object - WebGL2ContextVAOs.cpp
  // TODO(djg): Implemented in WebGLContext
  /*
      already_AddRefed<WebGLVertexArrayObject> CreateVertexArray();
      void DeleteVertexArray(WebGLVertexArrayObject* vertexArray);
      bool IsVertexArray(WebGLVertexArrayObject* vertexArray);
      void BindVertexArray(WebGLVertexArrayObject* vertexArray);
  */

 private:
  WebGL2Context();
  virtual UniquePtr<webgl::FormatUsageAuthority> CreateFormatUsage(
      gl::GLContext* gl) const override;

  virtual bool IsTexParamValid(GLenum pname) const override;

  void UpdateBoundQuery(GLenum target, WebGLQuery* query);

  // CreateVertexArrayImpl is assumed to be infallible.
  virtual WebGLVertexArray* CreateVertexArrayImpl() override;
};

}  // namespace mozilla

#endif
