#include "GLRender.h"
#include "EngineClient.h"
#include "ImageFileUtils.h"

#include <gl/glew.h> // Must be included before gl.h

#include <SDL_opengl.h> // Wraps OpenGL headers
#include <SDL_opengl_glext.h>

#include <gl/GLU.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat4x4.hpp>
#include <imgui.h>

#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

// Vectrex screen dimensions
const int VECTREX_SCREEN_WIDTH = 256;
const int VECTREX_SCREEN_HEIGHT = 256;

namespace {
    // Wraps an OpenGL resource id and automatically calls the delete function on destruction
    template <typename DeleteFunc>
    class GLResource {
    public:
        static const GLuint INVALID_RESOURCE_ID = ~0u;

        GLResource(GLuint id = INVALID_RESOURCE_ID, DeleteFunc deleteFunc = DeleteFunc{})
            : m_id(id)
            , m_deleteFunc(std::move(deleteFunc)) {}

        ~GLResource() {
            if (m_id != INVALID_RESOURCE_ID) {
                m_deleteFunc(m_id);
            }
        }

        // Disable copy
        GLResource(const GLResource&) = delete;
        GLResource& operator=(const GLResource&) = delete;

        // Enable move
        GLResource(GLResource&& rhs)
            : m_id(rhs.m_id)
            , m_deleteFunc(rhs.m_deleteFunc) {
            rhs.m_id = INVALID_RESOURCE_ID;
        }
        GLResource& operator=(GLResource&& rhs) {
            if (this != &rhs) {
                std::swap(m_id, rhs.m_id);
                std::swap(m_deleteFunc, m_deleteFunc);
            }
            return *this;
        }

        GLuint get() const {
            assert(m_id != INVALID_RESOURCE_ID);
            return m_id;
        }
        GLuint operator*() const { return get(); }

    private:
        GLuint m_id;
        DeleteFunc m_deleteFunc;
    };

    auto MakeTextureResource() {
        struct Deleter {
            auto operator()(GLuint id) { glDeleteTextures(1, &id); }
        };
        GLuint id;
        glGenTextures(1, &id);
        return GLResource<decltype(Deleter{})>{id};
    }
    using TextureResource = decltype(MakeTextureResource());

    auto MakeVertexArrayResource() {
        struct Deleter {
            auto operator()(GLuint id) { glDeleteVertexArrays(1, &id); }
        };
        GLuint id;
        glGenVertexArrays(1, &id);
        return GLResource<decltype(Deleter{})>{id};
    }
    using VertexArrayResource = decltype(MakeVertexArrayResource());

    auto MakeFrameBufferResource() {
        struct Deleter {
            auto operator()(GLuint id) { glDeleteFramebuffers(1, &id); }
        };
        GLuint id;
        glGenFramebuffers(1, &id);
        return GLResource<decltype(Deleter{})>{id};
    }
    using FrameBufferResource = decltype(MakeFrameBufferResource());

    auto MakeBufferResource() {
        struct Deleter {
            auto operator()(GLuint id) { glDeleteBuffers(1, &id); }
        };
        GLuint id;
        glGenBuffers(1, &id);
        return GLResource<decltype(Deleter{})>{id};
    }
    using BufferResource = decltype(MakeBufferResource());

    std::optional<std::string> FileToString(const char* file) {
        std::ifstream is(file);
        if (!is)
            return {};
        std::stringstream buffer;
        buffer << is.rdbuf();
        return buffer.str();
    }

    void SetVertexBufferData(GLuint vboId, const std::vector<glm::vec2>& vertices) {
        glBindBuffer(GL_ARRAY_BUFFER, vboId);
        glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(glm::vec2), vertices.data(),
                     GL_DYNAMIC_DRAW);
    }

    template <typename T, size_t N>
    void SetVertexBufferData(GLuint vboId, T (&vertices)[N]) {
        glBindBuffer(GL_ARRAY_BUFFER, vboId);
        glBufferData(GL_ARRAY_BUFFER, N * sizeof(vertices[0]), &vertices[0], GL_DYNAMIC_DRAW);
    }

    template <typename T, size_t N>
    void SetVertexBufferData(GLuint vboId, const std::array<T, N>& vertices) {
        glBindBuffer(GL_ARRAY_BUFFER, vboId);
        glBufferData(GL_ARRAY_BUFFER, N * sizeof(vertices[0]), &vertices[0], GL_DYNAMIC_DRAW);
    }

    void CheckFramebufferStatus() {
        ASSERT(glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE);
    }

    GLuint LoadShaders(const char* vertShaderFile, const char* fragShaderFile) {
        auto CheckStatus = [](GLuint id, GLenum pname) {
            assert(pname == GL_COMPILE_STATUS || pname == GL_LINK_STATUS);
            GLint result = GL_FALSE;
            int infoLogLength{};
            glGetShaderiv(id, pname, &result);
            glGetShaderiv(id, GL_INFO_LOG_LENGTH, &infoLogLength);
            if (infoLogLength > 0) {
                std::vector<char> info(infoLogLength + 1);
                if (pname == GL_COMPILE_STATUS)
                    glGetShaderInfoLog(id, infoLogLength, NULL, info.data());
                else
                    glGetProgramInfoLog(id, infoLogLength, NULL, info.data());
                printf("%s", info.data());
            }
            return true;
        };

        auto CompileShader = [CheckStatus](GLuint shaderId, const char* shaderFile) {
            auto shaderCode = FileToString(shaderFile);
            if (!shaderCode) {
                printf("Failed to open %s", shaderFile);
                return false;
            }
            printf("Compiling shader : %s\n", shaderFile);
            auto sourcePtr = shaderCode->c_str();
            glShaderSource(shaderId, 1, &sourcePtr, NULL);
            glCompileShader(shaderId);
            return CheckStatus(shaderId, GL_COMPILE_STATUS);
        };

        auto LinkProgram = [CheckStatus](GLuint programId) {
            printf("Linking program\n");
            glLinkProgram(programId);
            return CheckStatus(programId, GL_LINK_STATUS);
        };

        GLuint vertShaderId = glCreateShader(GL_VERTEX_SHADER);
        if (!CompileShader(vertShaderId, vertShaderFile))
            return 0;

        GLuint fragShaderId = glCreateShader(GL_FRAGMENT_SHADER);
        if (!CompileShader(fragShaderId, fragShaderFile))
            return 0;

        GLuint programId = glCreateProgram();
        glAttachShader(programId, vertShaderId);
        glAttachShader(programId, fragShaderId);
        if (!LinkProgram(programId))
            return 0;

        glDetachShader(programId, vertShaderId);
        glDetachShader(programId, fragShaderId);

        glDeleteShader(vertShaderId);
        glDeleteShader(fragShaderId);

        return programId;
    }

    void SetUniform(GLuint shader, const char* name, GLint value) {
        GLuint id = glGetUniformLocation(shader, name);
        glUniform1i(id, value);
    }
    void SetUniform(GLuint shader, const char* name, GLfloat value) {
        GLuint id = glGetUniformLocation(shader, name);
        glUniform1f(id, value);
    }
    void SetUniform(GLuint shader, const char* name, GLfloat value1, GLfloat value2) {
        GLuint id = glGetUniformLocation(shader, name);
        glUniform2f(id, value1, value2);
    }
    void SetUniform(GLuint shader, const char* name, GLfloat* values, GLsizei size) {
        GLuint id = glGetUniformLocation(shader, name);
        glUniform1fv(id, size, values);
    }

    constexpr GLenum TextureSlotToTextureEnum(GLint slot) {
        switch (slot) {
        case 0:
            return GL_TEXTURE0;
        case 1:
            return GL_TEXTURE1;
        case 2:
            return GL_TEXTURE2;
        case 3:
            return GL_TEXTURE3;
        default:
            assert(false);
            return 0;
        }
    }

    template <GLint textureSlot>
    void SetTextureUniform(GLuint shader, const char* name, GLuint textureId) {
        constexpr auto texEnum = TextureSlotToTextureEnum(textureSlot);
        glActiveTexture(texEnum);
        glBindTexture(GL_TEXTURE_2D, textureId);
        GLuint texLoc = glGetUniformLocation(shader, name);
        glUniform1i(texLoc, textureSlot);
    }

    struct PixelData {
        uint8_t* pixels{};
        GLenum format{};
        GLenum type{};
    };
    void AllocateTexture(GLuint textureId, GLsizei width, GLsizei height, GLint internalFormat,
                         std::optional<PixelData> pixelData = {}) {

        auto pd = pixelData.value_or(PixelData{nullptr, GL_RGBA, GL_UNSIGNED_BYTE});

        glBindTexture(GL_TEXTURE_2D, textureId);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, pd.format, pd.type,
                     pd.pixels);
        // By default, filtering is GL_LINEAR or GL_NEAREST_MIPMAP_LINEAR. We set to GL_NEAREST to
        // avoid creating mipmaps and to make it less blurry.
        // auto filtering = GL_LINEAR;
        auto filtering = GL_NEAREST;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filtering);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filtering);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    bool LoadPngTexture(GLuint textureId, const char* file) {
        auto imgData = ImageFileUtils::loadPngImage(file);

        if (!imgData) {
            return false;
        }

        AllocateTexture(
            textureId, imgData->width, imgData->height, imgData->hasAlpha ? GL_RGBA : GL_RGB,
            PixelData{imgData->data.get(),
                      static_cast<GLenum>(imgData->hasAlpha ? GL_RGBA : GL_RGB), GL_UNSIGNED_BYTE});
        return true;
    }

    struct Viewport {
        GLint x{}, y{};
        GLsizei w{}, h{};
    };
    Viewport GetBestFitViewport(float targetAR, int windowWidth, int windowHeight) {
        float windowWidthF = static_cast<float>(windowWidth);
        float windowHeightF = static_cast<float>(windowHeight);
        float windowAR = windowWidthF / windowHeightF;

        bool fitToWindowHeight = targetAR <= windowAR;

        const auto[targetWidth, targetHeight] = [&] {
            if (fitToWindowHeight) {
                // fit to window height
                return std::make_tuple(targetAR * windowHeightF, windowHeightF);
            } else {
                // fit to window width
                return std::make_tuple(windowWidthF, 1 / targetAR * windowWidthF);
            }
        }();

        return {static_cast<GLint>((windowWidthF - targetWidth) / 2.0f),
                static_cast<GLint>((windowHeightF - targetHeight) / 2.0f),
                static_cast<GLsizei>(targetWidth), static_cast<GLsizei>(targetHeight)};
    }

    struct VertexData {
        glm::vec2 v{};
        float brightness{};
    };

    std::vector<VertexData> CreateQuadVertexArray(const std::vector<Line>& lines) {
        auto AlmostEqual = [](float a, float b, float epsilon = 0.01f) {
            return abs(a - b) <= epsilon;
        };

        std::vector<VertexData> result;
        static float lineWidth = 1.0f;
        ImGui::SliderFloat("lineWidth", &lineWidth, 0.1f, 2.0f);

        float hlw = lineWidth / 2.0f;

        for (auto& line : lines) {
            glm::vec2 p0{line.p0.x, line.p0.y};
            glm::vec2 p1{line.p1.x, line.p1.y};

            if (AlmostEqual(p0.x, p1.x) && AlmostEqual(p0.y, p1.y)) {
                auto a = VertexData{p0 + glm::vec2{0, hlw}, line.brightness};
                auto b = VertexData{p0 - glm::vec2{0, hlw}, line.brightness};
                auto c = VertexData{p0 + glm::vec2{hlw, 0}, line.brightness};
                auto d = VertexData{p0 - glm::vec2{hlw, 0}, line.brightness};

                result.insert(result.end(), {a, b, c, c, d, a});

            } else {
                glm::vec2 v01 = glm::normalize(p1 - p0);
                glm::vec2 n(-v01.y, v01.x);

                auto a = VertexData{p0 + n * hlw, line.brightness};
                auto b = VertexData{p0 - n * hlw, line.brightness};
                auto c = VertexData{p1 - n * hlw, line.brightness};
                auto d = VertexData{p1 + n * hlw, line.brightness};

                result.insert(result.end(), {a, b, c, c, d, a});
            }
        }
        return result;
    }

    std::tuple<std::vector<VertexData>, std::vector<VertexData>>
    CreateLineAndPointVertexArrays(const std::vector<Line>& lines) {
        auto AlmostEqual = [](float a, float b, float epsilon = 0.01f) {
            return abs(a - b) <= epsilon;
        };

        std::vector<VertexData> lineVA, pointVA;

        for (auto& line : lines) {
            glm::vec2 p0{line.p0.x, line.p0.y};
            glm::vec2 p1{line.p1.x, line.p1.y};

            if (AlmostEqual(p0.x, p1.x) && AlmostEqual(p0.y, p1.y)) {
                pointVA.push_back({p0, line.brightness});
            } else {
                lineVA.push_back({p0, line.brightness});
                lineVA.push_back({p1, line.brightness});
            }
        }

        return {lineVA, pointVA};
    }

    // Globals
    float CRT_SCALE_X = 0.93f;
    float CRT_SCALE_Y = 0.76f;
    int g_windowWidth{}, g_windowHeight{};

    Viewport g_windowViewport{}, g_overlayViewport{}, g_crtViewport{};

    std::vector<Line> g_lines;
    std::vector<VertexData> g_quadVA;
    std::vector<VertexData> g_lineVA, g_pointVA;

    namespace ShaderProgram {
        GLuint drawVectors{};
        GLuint glow{};
        GLuint copyTexture{};
        GLuint darkenTexture{};
        GLuint gameScreenToCrtTexture{};
        GLuint drawScreen{};
    } // namespace ShaderProgram

    glm::mat4x4 g_projectionMatrix{};
    glm::mat4x4 g_modelViewMatrix{};
    VertexArrayResource g_topLevelVAO;
    FrameBufferResource g_textureFB;
    int g_vectorsTexture0Index{};
    TextureResource g_vectorsTexture0;
    TextureResource g_vectorsTexture1;
    TextureResource g_vectorsThickTexture0;
    TextureResource g_vectorsThickTexture1;
    TextureResource g_glowTexture0;
    TextureResource g_glowTexture1;
    TextureResource g_crtTexture;
    TextureResource g_overlayTexture;

} // namespace

namespace GLRender {
    void Initialize() {
        // glShadeModel(GL_SMOOTH);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        // glClearDepth(1.0f);
        // glEnable(GL_DEPTH_TEST);
        // glDepthFunc(GL_LEQUAL);
        // glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_NICEST);

        // Initialize GLEW
        glewExperimental = true; // Needed for core profile
        if (glewInit() != GLEW_OK) {
            FAIL_MSG("Failed to initialize GLEW");
        }

        // We don't actually use VAOs, but we must create one and bind it so that we can use VBOs
        g_topLevelVAO = MakeVertexArrayResource();
        glBindVertexArray(*g_topLevelVAO);

        // Load shaders
        //@TODO: Use PassThrough.vert for drawVectors as well (don't forget to pass UVs along)
        ShaderProgram::drawVectors =
            LoadShaders("shaders/DrawVectors.vert", "shaders/DrawVectors.frag");

        ShaderProgram::glow = LoadShaders("shaders/PassThrough.vert", "shaders/Glow.frag");

        ShaderProgram::copyTexture =
            LoadShaders("shaders/PassThrough.vert", "shaders/CopyTexture.frag");

        ShaderProgram::darkenTexture =
            LoadShaders("shaders/PassThrough.vert", "shaders/DarkenTexture.frag");

        ShaderProgram::gameScreenToCrtTexture =
            LoadShaders("shaders/Passthrough.vert", "shaders/DrawTexture.frag");

        ShaderProgram::drawScreen =
            LoadShaders("shaders/Passthrough.vert", "shaders/DrawScreen.frag");

        // Create resources
        g_textureFB = MakeFrameBufferResource();
        // Set output of fragment shader to color attachment 0
        glBindFramebuffer(GL_FRAMEBUFFER, *g_textureFB);
        GLenum drawBuffers[1] = {GL_COLOR_ATTACHMENT0};
        glDrawBuffers(1, drawBuffers);
        CheckFramebufferStatus();

        // For now, always attempt to load the mine storm overlay
        g_overlayTexture = MakeTextureResource();
        glObjectLabel(GL_TEXTURE, *g_overlayTexture, -1, "g_overlayTexture");
        const char* overlayFile = "overlays/mine.png";
        if (!LoadPngTexture(*g_overlayTexture, overlayFile)) {
            std::cerr << "Failed to load overlay: " << overlayFile << "\n";

            // If we fail, then allocate a min-sized transparent texture
            std::vector<uint8_t> emptyTexture;
            emptyTexture.resize(64 * 64 * 4);
            AllocateTexture(*g_overlayTexture, 64, 64, GL_RGBA,
                            PixelData{&emptyTexture[0], GL_RGBA, GL_UNSIGNED_BYTE});
        }
    }

    void Shutdown() {
        //
    }

    std::tuple<int, int> GetMajorMinorVersion() { return {3, 3}; }

    void SetViewport(const Viewport& vp) {
        glViewport((GLint)vp.x, (GLint)vp.y, (GLsizei)vp.w, (GLsizei)vp.h);
    }

    bool OnWindowResized(int windowWidth, int windowHeight) {
        if (windowHeight == 0) {
            windowHeight = 1;
        }
        g_windowWidth = windowWidth;
        g_windowHeight = windowHeight;

        // Overlay
        float overlayAR = 936.f / 1200.f;
        g_windowViewport = GetBestFitViewport(overlayAR, windowWidth, windowHeight);
        g_overlayViewport = {0, 0, g_windowViewport.w, g_windowViewport.h};
        g_crtViewport = {0, 0, static_cast<GLsizei>(g_overlayViewport.w * CRT_SCALE_X),
                         static_cast<GLsizei>(g_overlayViewport.h * CRT_SCALE_Y)};

        SetViewport(g_overlayViewport);

        double halfWidth = VECTREX_SCREEN_WIDTH / 2.0;
        double halfHeight = VECTREX_SCREEN_HEIGHT / 2.0;
        g_projectionMatrix = glm::ortho(-halfWidth, halfWidth, -halfHeight, halfHeight);

        g_modelViewMatrix = {};

        // (Re)create resources that depend on viewport size
        g_vectorsTexture0 = MakeTextureResource();
        AllocateTexture(*g_vectorsTexture0, g_crtViewport.w, g_crtViewport.h, GL_RGB32F);

        g_vectorsTexture1 = MakeTextureResource();
        AllocateTexture(*g_vectorsTexture1, g_crtViewport.w, g_crtViewport.h, GL_RGB32F);

        g_vectorsThickTexture0 = MakeTextureResource();
        AllocateTexture(*g_vectorsThickTexture0, g_crtViewport.w, g_crtViewport.h, GL_RGB32F);

        g_vectorsThickTexture1 = MakeTextureResource();
        AllocateTexture(*g_vectorsThickTexture1, g_crtViewport.w, g_crtViewport.h, GL_RGB32F);

        g_glowTexture0 = MakeTextureResource();
        glObjectLabel(GL_TEXTURE, *g_glowTexture0, -1, "g_glowTexture0");
        AllocateTexture(*g_glowTexture0, g_crtViewport.w, g_crtViewport.h, GL_RGB32F);

        g_glowTexture1 = MakeTextureResource();
        glObjectLabel(GL_TEXTURE, *g_glowTexture1, -1, "g_glowTexture1");
        AllocateTexture(*g_glowTexture1, g_crtViewport.w, g_crtViewport.h, GL_RGB32F);

        g_crtTexture = MakeTextureResource();
        AllocateTexture(*g_crtTexture, g_overlayViewport.w, g_overlayViewport.h, GL_RGB);
        glObjectLabel(GL_TEXTURE, *g_crtTexture, -1, "g_crtTexture");

        // Clear g_vectorsTexture0 once
        glBindFramebuffer(GL_FRAMEBUFFER, *g_textureFB);
        glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, *g_vectorsTexture0, 0);
        SetViewport(g_overlayViewport);
        glClear(GL_COLOR_BUFFER_BIT);

        return true;
    }

    void RenderScene(double frameTime) {
        // Force resize on crt scale change
        {
            static float scaleX = CRT_SCALE_X;
            static float scaleY = CRT_SCALE_Y;
            ImGui::SliderFloat("scaleX", &scaleX, 0.0f, 1.0f);
            ImGui::SliderFloat("scaleY", &scaleY, 0.0f, 1.0f);

            if (scaleX != CRT_SCALE_X || scaleY != CRT_SCALE_Y) {
                CRT_SCALE_X = scaleX;
                CRT_SCALE_Y = scaleY;
                OnWindowResized(g_windowWidth, g_windowHeight);
            }
        }

        auto MakeClipSpaceQuad = [](float scaleX = 1.f, float scaleY = 1.f) {
            std::array<glm::vec3, 6> quad_vertices{
                glm::vec3{-scaleX, -scaleY, 0.0f}, glm::vec3{scaleX, -scaleY, 0.0f},
                glm::vec3{-scaleX, scaleY, 0.0f},  glm::vec3{-scaleX, scaleY, 0.0f},
                glm::vec3{scaleX, -scaleY, 0.0f},  glm::vec3{scaleX, scaleY, 0.0f}};
            return quad_vertices;
        };

        auto DrawFullScreenQuad = [MakeClipSpaceQuad](float scaleX = 1.f, float scaleY = 1.f) {
            //@TODO: create once
            auto vbo = MakeBufferResource();
            auto quad_vertices = MakeClipSpaceQuad(scaleX, scaleY);
            SetVertexBufferData(*vbo, quad_vertices);

            glEnableVertexAttribArray(0);
            glBindBuffer(GL_ARRAY_BUFFER, *vbo);
            glVertexAttribPointer(0, // attribute 0. No particular reason for 0, but must match
                                     // layout in the shader.
                                  3, // size
                                  GL_FLOAT, // type
                                  GL_FALSE, // normalized?
                                  0,        // stride
                                  (void*)0  // array buffer offset
            );

            glm::vec2 quad_uvs[] = {{0, 0}, {1, 0}, {0, 1}, {0, 1}, {1, 0}, {1, 1}};
            auto uvBuffer = MakeBufferResource();
            SetVertexBufferData(*uvBuffer, quad_uvs);

            // 2nd attribute buffer : UVs
            glEnableVertexAttribArray(1);
            glBindBuffer(GL_ARRAY_BUFFER, *uvBuffer);
            glVertexAttribPointer(1,        // attribute
                                  2,        // size
                                  GL_FLOAT, // type
                                  GL_FALSE, // normalized?
                                  0,        // stride
                                  (void*)0  // array buffer offset
            );

            glDrawArrays(GL_TRIANGLES, 0, 6); // 2*3 indices starting at 0 -> 2 triangles

            glDisableVertexAttribArray(0);
            glDisableVertexAttribArray(1);
        };

        /////////////////////////////////////////////////////////////////
        // PASS 1: draw vectors
        /////////////////////////////////////////////////////////////////
        auto DrawVectors = [&](const std::vector<VertexData>& VA1, GLenum mode1,
                               const std::vector<VertexData>& VA2, GLenum mode2,
                               GLuint targetTextureId) {

            // Render to our framebuffer
            glBindFramebuffer(GL_FRAMEBUFFER, *g_textureFB);
            SetViewport(g_crtViewport); //@TODO: maybe get Viewport from texture
                                        // dimensions? ({0,0,texW,textH})
            glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, targetTextureId, 0);

            // Purposely do not clear the target texture.
            // glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            // Use our shader
            glUseProgram(ShaderProgram::drawVectors);

            const auto mvp = g_projectionMatrix * g_modelViewMatrix;
            GLuint mvpUniform = glGetUniformLocation(ShaderProgram::drawVectors, "MVP");
            glUniformMatrix4fv(mvpUniform, 1, GL_FALSE, &mvp[0][0]);

            auto DrawVertices = [](auto& VA, GLenum mode) {
                if (VA.size() == 0)
                    return;

                auto vbo = MakeBufferResource();

                glBindBuffer(GL_ARRAY_BUFFER, *vbo);
                glBufferData(GL_ARRAY_BUFFER, VA.size() * sizeof(VertexData), VA.data(),
                             GL_DYNAMIC_DRAW);

                glEnableVertexAttribArray(0);
                glEnableVertexAttribArray(1);

                // Vertices
                glVertexAttribPointer(0,                             // attribute
                                      2,                             // size
                                      GL_FLOAT,                      // type
                                      GL_FALSE,                      // normalized?
                                      sizeof(VertexData),            // stride
                                      (void*)offsetof(VertexData, v) // array buffer offset
                );

                // Brightness values
                glVertexAttribPointer(1,                                      // attribute
                                      1,                                      // size
                                      GL_FLOAT,                               // type
                                      GL_FALSE,                               // normalized?
                                      sizeof(VertexData),                     // stride
                                      (void*)offsetof(VertexData, brightness) // array buffer offset
                );

                glDrawArrays(mode, 0, VA.size());

                glDisableVertexAttribArray(1);
                glDisableVertexAttribArray(0);
            };

            DrawVertices(VA1, mode1);
            DrawVertices(VA2, mode2);
        };

        /////////////////////////////////////////////////////////////////
        // PASS 2: darken texture
        /////////////////////////////////////////////////////////////////
        auto DarkenTexture = [&](GLuint inputTextureId, GLuint outputTextureId) {
            GLuint shader = ShaderProgram::darkenTexture;

            glBindFramebuffer(GL_FRAMEBUFFER, *g_textureFB);
            SetViewport(g_crtViewport);
            glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, outputTextureId, 0);

            glClear(GL_COLOR_BUFFER_BIT);

            glUseProgram(shader);

            // Bind our texture in Texture Unit 0
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, inputTextureId);

            // Set our "vectorsTexture0" sampler to use Texture Unit 0
            SetUniform(shader, "vectorsTexture", 0);

            static float darkenSpeedScale = 3.0;
            ImGui::SliderFloat("darkenSpeedScale", &darkenSpeedScale, 0.0f, 10.0f);
            SetUniform(shader, "darkenSpeedScale", darkenSpeedScale);

            SetUniform(shader, "frameTime", (float)(frameTime));

            DrawFullScreenQuad();
        };

        // GLOW
        auto ApplyGlow = [&](GLuint inputTextureId, GLuint tempTextureId, GLuint outputTextureId) {

            static float radius = 3.0f;
            ImGui::SliderFloat("glowRadius", &radius, 0.0f, 5.0f);

            static std::array<float, 5> glowKernelValues = {
                0.2270270270f, 0.1945945946f, 0.1216216216f, 0.0540540541f, 0.0162162162f,
            };

            for (size_t i = 0; i < glowKernelValues.size(); ++i) {
                ImGui::SliderFloat(FormattedString<>("kernelValue[%d]", i), &glowKernelValues[i],
                                   0.f, 1.f);
            }

            auto GlowInDirection = [&](GLuint inputTextureId, GLuint outputTextureId,
                                       glm::vec2 dir) {
                GLuint shader = ShaderProgram::glow;

                glBindFramebuffer(GL_FRAMEBUFFER, *g_textureFB);
                SetViewport(g_crtViewport);
                glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, outputTextureId, 0);

                glUseProgram(shader);

                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, inputTextureId);
                SetUniform(shader, "inputTexture", 0);

                SetUniform(shader, "dir", dir.x, dir.y);
                SetUniform(shader, "resolution",
                           static_cast<float>(std::min(g_crtViewport.w, g_crtViewport.h)));
                SetUniform(shader, "radius", radius);
                SetUniform(shader, "kernalValues", &glowKernelValues[0], glowKernelValues.size());

                DrawFullScreenQuad();
            };

            GlowInDirection(inputTextureId, tempTextureId, {1.f, 0.f});
            GlowInDirection(tempTextureId, outputTextureId, {0.f, 1.f});
        };

        /////////////////////////////////////////////////////////////////
        // PASS 3: render game screen texture to crt texture that is
        //         larger (same size as overlay texture)
        /////////////////////////////////////////////////////////////////
        auto GameScreenToCrtTexture = [&](GLuint inputTextureId, GLuint outputTextureId) {

            GLuint shader = ShaderProgram::gameScreenToCrtTexture;

            glBindFramebuffer(GL_FRAMEBUFFER, *g_textureFB);
            SetViewport(g_overlayViewport);
            glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, outputTextureId, 0);

            glClear(GL_COLOR_BUFFER_BIT);

            glUseProgram(shader);

            // Bind our texture in Texture Unit 0
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, inputTextureId);
            // Set our "vectorsTexture0" sampler to use Texture Unit 0
            SetUniform(shader, "vectorsTexture", 0);

            DrawFullScreenQuad(CRT_SCALE_X, CRT_SCALE_Y);
        };

        /////////////////////////////////////////////////////////////////
        // PASS 4: render to screen
        /////////////////////////////////////////////////////////////////
        auto RenderToScreen = [&](GLuint inputCrtTextureId, GLuint inputOverlayTextureId) {

            GLuint shader = ShaderProgram::drawScreen;

            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            SetViewport(g_windowViewport);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            // Use our shader
            glUseProgram(shader);

            SetTextureUniform<0>(shader, "crtTexture", inputCrtTextureId);
            SetTextureUniform<1>(shader, "overlayTexture", inputOverlayTextureId);

            static float overlayAlpha = 1.0f;
            ImGui::SliderFloat("overlayAlpha", &overlayAlpha, 0.0f, 1.0f);
            SetUniform(shader, "overlayAlpha", overlayAlpha);

            DrawFullScreenQuad();
        };

        if (frameTime > 0)
            g_vectorsTexture0Index = (g_vectorsTexture0Index + 1) % 2;

        auto& currVectorsTexture0 =
            g_vectorsTexture0Index == 0 ? g_vectorsTexture0 : g_vectorsTexture1;
        auto& currVectorsTexture1 =
            g_vectorsTexture0Index == 0 ? g_vectorsTexture1 : g_vectorsTexture0;

        auto& currVectorsThickTexture0 =
            g_vectorsTexture0Index == 0 ? g_vectorsThickTexture0 : g_vectorsThickTexture1;
        auto& currVectorsThickTexture1 =
            g_vectorsTexture0Index == 0 ? g_vectorsThickTexture1 : g_vectorsThickTexture0;

        glObjectLabel(GL_TEXTURE, *currVectorsTexture0, -1, "currVectorsTexture0");
        glObjectLabel(GL_TEXTURE, *currVectorsTexture1, -1, "currVectorsTexture1");

        glObjectLabel(GL_TEXTURE, *currVectorsThickTexture0, -1, "currVectorsThickTexture0");
        glObjectLabel(GL_TEXTURE, *currVectorsThickTexture1, -1, "currVectorsThickTexture1");

        // Render normal lines and points and darken
        std::tie(g_lineVA, g_pointVA) = CreateLineAndPointVertexArrays(g_lines);
        DrawVectors(g_lineVA, GL_LINES, g_pointVA, GL_POINTS, *currVectorsTexture0);
        DarkenTexture(*currVectorsTexture0, *currVectorsTexture1);

        // Render thicker lines for blurring, darken, and apply glow
        g_quadVA = CreateQuadVertexArray(g_lines);
        DrawVectors(g_quadVA, GL_TRIANGLES, {}, {}, *currVectorsThickTexture0);
        DarkenTexture(*currVectorsThickTexture0, *currVectorsThickTexture1);
        ApplyGlow(*currVectorsThickTexture0, *g_glowTexture0, *g_glowTexture1);

        // Combine glow and normal lines
        //@TODO: Write shader and code for combining the two

        GameScreenToCrtTexture(*currVectorsTexture0, *g_crtTexture);

        RenderToScreen(*g_crtTexture, *g_overlayTexture);
    }

} // namespace GLRender

void Display::Clear() {
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void Display::DrawLines(const std::vector<Line>& lines) {
    g_lines = lines; //@TODO: move?
}
