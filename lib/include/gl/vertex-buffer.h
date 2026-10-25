#ifndef _EDGE_LIGHTING_VERTEX_BUFFER_H_
#define _EDGE_LIGHTING_VERTEX_BUFFER_H_

#include "gl/gl-header.h"
#include "gl/vertex-array.h"
#include "util/log-util.h"
#include <string>

namespace EdgeLighting
{
    /// RAII wrapper for a vertex buffer object that feeds attributes of a
    /// @ref VertexArray it does not own.
    ///
    /// @ref VertexArray is a VAO plus the one VBO its attributes read. This is
    /// a SECOND buffer bound into such a VAO, for a module that keeps part of
    /// the per-vertex data apart from the owner's vertex struct - the
    /// spotlight shadow's per-lamp attributes beside SpotlightRenderer's
    /// strips. The VAO records which buffer each attribute reads at
    /// @ref AttachAttrib, so the two buffers can be refilled independently.
    /// Move-only.
    class VertexBuffer
    {
    public:
        /// Generates a new VBO.
        /// @param name  Optional label used in log messages (default "unnamed").
        VertexBuffer(const char *name = nullptr)
            : mName(name ? name : "unnamed")
        {
            glGenBuffers(1, &mVbo);
            LOG_I("VertexBuffer[%s] created (vbo=%u).", mName.c_str(), mVbo);
        }

        /// Deletes the VBO.
        ~VertexBuffer()
        {
            if (mVbo != 0)
            {
                glDeleteBuffers(1, &mVbo);
            }
            LOG_I("VertexBuffer[%s] destroyed.", mName.c_str());
        }

        VertexBuffer(const VertexBuffer &) = delete;
        VertexBuffer &operator=(const VertexBuffer &) = delete;

        VertexBuffer(VertexBuffer &&other) noexcept
            : mVbo(other.mVbo), mName(std::move(other.mName))
        {
            other.mVbo = 0;
        }

        VertexBuffer &operator=(VertexBuffer &&other) noexcept
        {
            if (this != &other)
            {
                if (mVbo != 0)
                {
                    glDeleteBuffers(1, &mVbo);
                }
                mVbo = other.mVbo;
                mName = std::move(other.mName);
                other.mVbo = 0;
            }
            return *this;
        }

        /// (Re)allocates the buffer and optionally fills it.
        /// @param data   Source data (nullptr to allocate without uploading).
        /// @param size   Size in bytes.
        /// @param usage  GL_STATIC_DRAW, GL_DYNAMIC_DRAW, etc.
        void SetData(const void *data, size_t size, GLenum usage = GL_STATIC_DRAW) const
        {
            glBindBuffer(GL_ARRAY_BUFFER, mVbo);
            glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(size), data, usage);
        }

        /// Overwrites @p size bytes at @p offset of an allocation
        /// @ref SetData made.
        void SetSubData(size_t offset, size_t size, const void *data) const
        {
            glBindBuffer(GL_ARRAY_BUFFER, mVbo);
            glBufferSubData(GL_ARRAY_BUFFER, static_cast<GLintptr>(offset),
                            static_cast<GLsizeiptr>(size), data);
        }

        /// Points attribute @p location of @p vao at this buffer and enables
        /// it. Binds @p vao, and leaves it bound, as
        /// @ref VertexArray::SetAttribPointer does.
        /// @param location  Shader layout location index.
        /// @param size      Number of components (1-4).
        /// @param type      GL_FLOAT, etc.
        /// @param stride    Byte stride between consecutive vertices.
        /// @param offset    Byte offset to the first attribute value.
        void AttachAttrib(const VertexArray &vao, GLuint location, GLint size, GLenum type,
                          GLsizei stride, size_t offset) const
        {
            vao.Bind();
            glBindBuffer(GL_ARRAY_BUFFER, mVbo);
            glEnableVertexAttribArray(location);
            glVertexAttribPointer(location, size, type, GL_FALSE, stride, (void *)offset);
        }

    private:
        GLuint mVbo = 0; ///< Vertex Buffer Object handle.
        std::string mName = "unnamed";
    };

} // namespace EdgeLighting

#endif // _EDGE_LIGHTING_VERTEX_BUFFER_H_
