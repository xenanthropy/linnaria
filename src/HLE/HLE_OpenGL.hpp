#pragma once
#include "Config.hpp"
#include "SyscallRouter.hpp"
#include "GuestMemory.hpp"
#include "Pacing.hpp"
#include <glad/gles2.h>

#include <zlib.h>

namespace HLE::OpenGL {

    inline void RegisterAll(SyscallRouter& router, GuestMemory& memory) {

        // TODO: 

        auto gl_void_stub = [](Dynarmic::A32::Jit* cpu) {
            // Do nothing
        };

        ROUTE_REGISTER(router, "glClear", [](Dynarmic::A32::Jit* cpu) {
            glClear(cpu->Regs()[0]);
            Pacing::frame_dirty.store(true, std::memory_order_relaxed);
        });
        ROUTE_REGISTER(router, "glEnable", [](Dynarmic::A32::Jit* cpu) { glEnable(cpu->Regs()[0]); });
        ROUTE_REGISTER(router, "glDisable", [](Dynarmic::A32::Jit* cpu) { glDisable(cpu->Regs()[0]); });
        ROUTE_REGISTER(router, "glDepthMask", [](Dynarmic::A32::Jit* cpu) { glDepthMask(cpu->Regs()[0]); });
        ROUTE_REGISTER(router, "glUseProgram", [](Dynarmic::A32::Jit* cpu) { glUseProgram(cpu->Regs()[0]); });
        ROUTE_REGISTER(router, "glCompileShader", [](Dynarmic::A32::Jit* cpu) { glCompileShader(cpu->Regs()[0]); });
        ROUTE_REGISTER(router, "glLinkProgram", [](Dynarmic::A32::Jit* cpu) { glLinkProgram(cpu->Regs()[0]); });
        ROUTE_REGISTER(router, "glActiveTexture", [](Dynarmic::A32::Jit* cpu) { glActiveTexture(cpu->Regs()[0]); });
        ROUTE_REGISTER(router, "glBindTexture", [](Dynarmic::A32::Jit* cpu) { glBindTexture(cpu->Regs()[0], cpu->Regs()[1]); });
        ROUTE_REGISTER(router, "glBlendFunc", [](Dynarmic::A32::Jit* cpu) { glBlendFunc(cpu->Regs()[0], cpu->Regs()[1]); });
        ROUTE_REGISTER(router, "glAttachShader", [](Dynarmic::A32::Jit* cpu) { glAttachShader(cpu->Regs()[0], cpu->Regs()[1]); });
        ROUTE_REGISTER(router, "glUniform1i", [](Dynarmic::A32::Jit* cpu) { glUniform1i(cpu->Regs()[0], cpu->Regs()[1]); });
        ROUTE_REGISTER(router, "glTexParameteri", [](Dynarmic::A32::Jit* cpu) { glTexParameteri(cpu->Regs()[0], cpu->Regs()[1], cpu->Regs()[2]); });
        ROUTE_REGISTER(router, "glCreateShader", [](Dynarmic::A32::Jit* cpu) { cpu->Regs()[0] = glCreateShader(cpu->Regs()[0]); });
        ROUTE_REGISTER(router, "glCreateProgram", [](Dynarmic::A32::Jit* cpu) { cpu->Regs()[0] = glCreateProgram(); });
        ROUTE_REGISTER(router, "glDetachShader", [](Dynarmic::A32::Jit* cpu) { glDetachShader(cpu->Regs()[0], cpu->Regs()[1]); });
        ROUTE_REGISTER(router, "glDeleteShader", [](Dynarmic::A32::Jit* cpu) { glDeleteShader(cpu->Regs()[0]); });
        ROUTE_REGISTER(router, "glBindBuffer", [](Dynarmic::A32::Jit* cpu) { glBindBuffer(cpu->Regs()[0], cpu->Regs()[1]); });
        ROUTE_REGISTER(router, "glIsProgram", [](Dynarmic::A32::Jit* cpu) { cpu->Regs()[0] = glIsProgram(cpu->Regs()[0]); });
        ROUTE_REGISTER(router, "glIsTexture", [](Dynarmic::A32::Jit* cpu) { cpu->Regs()[0] = glIsTexture(cpu->Regs()[0]); });
        ROUTE_REGISTER(router, "glEnableVertexAttribArray",[](Dynarmic::A32::Jit* cpu) { glEnableVertexAttribArray(cpu->Regs()[0]); });
        ROUTE_REGISTER(router, "glPixelStorei",[](Dynarmic::A32::Jit* cpu) { glPixelStorei(cpu->Regs()[0], cpu->Regs()[1]); });


        ROUTE_REGISTER(router, "glGenTextures", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t n = cpu->Regs()[0];
            uint32_t ptr = cpu->Regs()[1];
            glGenTextures(n, reinterpret_cast<GLuint*>(memory.GetHostPointer(ptr)));
        });

        ROUTE_REGISTER(router, "glDeleteTextures", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t n = cpu->Regs()[0];
            uint32_t ptr = cpu->Regs()[1];
            glDeleteTextures(n, reinterpret_cast<const GLuint*>(memory.GetHostPointer(ptr)));
        });

        ROUTE_REGISTER(router, "glBufferData", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t target = cpu->Regs()[0];
            uint32_t size = cpu->Regs()[1];
            uint32_t data_ptr = cpu->Regs()[2];
            uint32_t usage = cpu->Regs()[3];
            
            const void* host_data = data_ptr ? memory.GetHostPointer(data_ptr) : nullptr;
            glBufferData(target, size, host_data, usage);
        });

        ROUTE_REGISTER(router, "glShaderSource", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t shader = cpu->Regs()[0];
            uint32_t count = cpu->Regs()[1];
            uint32_t string_array_ptr = cpu->Regs()[2];
            uint32_t length_array_ptr = cpu->Regs()[3];

            std::vector<const GLchar*> strings(count);
            for(uint32_t i = 0; i < count; i++) {
                uint32_t str_ptr = memory.Read32(string_array_ptr + (i * 4));
                strings[i] = reinterpret_cast<const GLchar*>(memory.GetHostPointer(str_ptr));
            }

            const GLint* lengths = length_array_ptr ? reinterpret_cast<const GLint*>(memory.GetHostPointer(length_array_ptr)) : nullptr;
            glShaderSource(shader, count, strings.data(), lengths);
        });

        ROUTE_REGISTER(router, "glTexImage2D", [&memory](Dynarmic::A32::Jit* cpu) {
            // Args 1-4 are in the registers
            uint32_t target = cpu->Regs()[0];
            uint32_t level = cpu->Regs()[1];
            uint32_t internalformat = cpu->Regs()[2];
            uint32_t width = cpu->Regs()[3];

            // Args 5-9 are on the Stack! (SP is R13)
            uint32_t sp = cpu->Regs()[13];
            uint32_t height = memory.Read32(sp);
            uint32_t border = memory.Read32(sp + 4);
            uint32_t format = memory.Read32(sp + 8);
            uint32_t type = memory.Read32(sp + 12);
            uint32_t pixels_ptr = memory.Read32(sp + 16);

            const void* host_pixels = pixels_ptr ? memory.GetHostPointer(pixels_ptr) : nullptr;

            // Push all 9 to your desktop GPU!
            glTexImage2D(target, level, internalformat, width, height, border, format, type, host_pixels);
        });

        // --- 1. Float Bitcasting (The Clear Functions) ---
        // ARM32 passes floats in the raw integer registers. We must bitcast them back to C++ floats!
        ROUTE_REGISTER(router, "glClearColor", [](Dynarmic::A32::Jit* cpu) {
            float r, g, b, a;
            std::memcpy(&r, &cpu->Regs()[0], 4);
            std::memcpy(&g, &cpu->Regs()[1], 4);
            std::memcpy(&b, &cpu->Regs()[2], 4);
            std::memcpy(&a, &cpu->Regs()[3], 4);
            glClearColor(r, g, b, a);
        });

        ROUTE_REGISTER(router, "glClearDepthf", [](Dynarmic::A32::Jit* cpu) {
            float depth;
            std::memcpy(&depth, &cpu->Regs()[0], 4);
            glClearDepthf(depth); 
        });

        // --- 3. Pointer Translations ---
        // Arrays and returned values must be mapped to your host's RAM.
        ROUTE_REGISTER(router, "glGenBuffers", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t n = cpu->Regs()[0];
            uint32_t buffers_ptr = cpu->Regs()[1];
            GLuint* host_buffers = reinterpret_cast<GLuint*>(memory.GetHostPointer(buffers_ptr));
            glGenBuffers(n, host_buffers);
        });

        ROUTE_REGISTER(router, "glGetIntegerv", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t pname = cpu->Regs()[0];
            uint32_t params_ptr = cpu->Regs()[1];
            GLint* host_params = reinterpret_cast<GLint*>(memory.GetHostPointer(params_ptr));
            
            // Your real desktop GPU will now answer things like "What is the max texture size?"
            glGetIntegerv(pname, host_params);
        });

        ROUTE_REGISTER(router, "glUniformMatrix4fv", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t location = cpu->Regs()[0];
            uint32_t count = cpu->Regs()[1];
            uint32_t transpose = cpu->Regs()[2];
            uint32_t value_ptr = cpu->Regs()[3];
            
            const GLfloat* host_value = reinterpret_cast<const GLfloat*>(memory.GetHostPointer(value_ptr));
            glUniformMatrix4fv(location, count, transpose, host_value);
        });

        // --- 4. String Translations ---
        ROUTE_REGISTER(router, "glGetUniformLocation", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t program = cpu->Regs()[0];
            uint32_t name_ptr = cpu->Regs()[1];
            const GLchar* name = reinterpret_cast<const GLchar*>(memory.GetHostPointer(name_ptr));
            
            cpu->Regs()[0] = glGetUniformLocation(program, name);
        });

        ROUTE_REGISTER(router, "glGetAttribLocation", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t program = cpu->Regs()[0];
            uint32_t name_ptr = cpu->Regs()[1];
            const GLchar* name = reinterpret_cast<const GLchar*>(memory.GetHostPointer(name_ptr));
            
            cpu->Regs()[0] = glGetAttribLocation(program, name);
        });


        // --- 5. The VBO Offset Trap (glDrawElements) ---
        ROUTE_REGISTER(router, "glDrawElements", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t mode = cpu->Regs()[0];
            uint32_t count = cpu->Regs()[1];
            uint32_t type = cpu->Regs()[2];
            uint32_t indices_val = cpu->Regs()[3];

            // TRICKY: If a game uses a Vertex Buffer Object (VBO), 'indices_val' is NOT a pointer. 
            // It is just an integer byte offset (e.g., 0, 12, 24).
            // But if it DOESN'T use a VBO, it's a real guest memory pointer that we have to translate!
            // We can safely guess: If the value is huge (like an 0x40000000 RAM address), it's a pointer.
            // If it's small, it's a VBO offset.
            
            const void* host_indices;
            if (indices_val > 0x100000) {
                // It's a raw pointer to guest RAM
                host_indices = memory.GetHostPointer(indices_val);
            } else {
                // It's a VBO offset. OpenGL expects us to cast the integer directly to a void pointer!
                host_indices = reinterpret_cast<const void*>(static_cast<uintptr_t>(indices_val));
            }

            glDrawElements(mode, count, type, host_indices);
            Pacing::frame_dirty.store(true, std::memory_order_relaxed);
        });

        // The Geometry Shipper
        ROUTE_REGISTER(router, "glVertexAttribPointer", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t index = cpu->Regs()[0];
            uint32_t size = cpu->Regs()[1];
            uint32_t type = cpu->Regs()[2];
            uint32_t normalized = cpu->Regs()[3];
    
            // Read Args 5 and 6 from the Stack
            uint32_t sp = cpu->Regs()[13];
            uint32_t stride = memory.Read32(sp);
            uint32_t pointer_val = memory.Read32(sp + 4);

            // VBO Offset vs RAM Pointer logic
            const void* host_ptr;
            if (pointer_val > 0x100000) host_ptr = memory.GetHostPointer(pointer_val);
            else host_ptr = reinterpret_cast<const void*>(static_cast<uintptr_t>(pointer_val));

            glVertexAttribPointer(index, size, type, normalized, stride, host_ptr);
        });

        // The Texture Updater
        ROUTE_REGISTER(router, "glTexSubImage2D", [&memory](Dynarmic::A32::Jit* cpu) {
            uint32_t target = cpu->Regs()[0];
            uint32_t level = cpu->Regs()[1];
            uint32_t xoffset = cpu->Regs()[2];
            uint32_t yoffset = cpu->Regs()[3];
    
            // Read Args 5-9 from the Stack
            uint32_t sp = cpu->Regs()[13];
            uint32_t width = memory.Read32(sp);
            uint32_t height = memory.Read32(sp + 4);
            uint32_t format = memory.Read32(sp + 8);
            uint32_t type = memory.Read32(sp + 12);
            uint32_t pixels_val = memory.Read32(sp + 16);
    
            const void* host_pixels = pixels_val ? memory.GetHostPointer(pixels_val) : nullptr;
    
            glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, host_pixels);
        });

        if (Config::GameSettings::Upscale::upscaling) {
            ROUTE_REGISTER(router, "glViewport", [](Dynarmic::A32::Jit* cpu) {
                GLint x = static_cast<GLint>(cpu->Regs()[0]);
                GLint y = static_cast<GLint>(cpu->Regs()[1]);
                GLsizei width = static_cast<GLsizei>(cpu->Regs()[2]);
                GLsizei height = static_cast<GLsizei>(cpu->Regs()[3]);

                GLint current_fbo;
                ::glGetIntegerv(GL_FRAMEBUFFER_BINDING, &current_fbo);

                // Only upscale if rendering directly to the screen (backbuffer)
                if (current_fbo == 0) {
                    float scale_x = Config::GameSettings::Upscale::upscaleWidth / Config::GameSettings::Resolution::nativeX;
                    float scale_y = Config::GameSettings::Upscale::upscaleHeight / Config::GameSettings::Resolution::nativeY;

                    ::glViewport(
                        static_cast<GLint>(x * scale_x),
                        static_cast<GLint>(y * scale_y),
                        static_cast<GLsizei>(width * scale_x),
                        static_cast<GLsizei>(height * scale_y)
                    );
                } else {
                    ::glViewport(x, y, width, height);
                }
            });

            ROUTE_REGISTER(router, "glScissor", [](Dynarmic::A32::Jit* cpu) {
                GLint x = static_cast<GLint>(cpu->Regs()[0]);
                GLint y = static_cast<GLint>(cpu->Regs()[1]);
                GLsizei width = static_cast<GLsizei>(cpu->Regs()[2]);
                GLsizei height = static_cast<GLsizei>(cpu->Regs()[3]);

                GLint current_fbo;
                ::glGetIntegerv(GL_FRAMEBUFFER_BINDING, &current_fbo);

                if (current_fbo == 0) {
                    float scale_x = Config::GameSettings::Upscale::upscaleWidth / Config::GameSettings::Resolution::nativeX;
                    float scale_y = Config::GameSettings::Upscale::upscaleHeight / Config::GameSettings::Resolution::nativeY;

                    ::glScissor(
                        static_cast<GLint>(x * scale_x),
                        static_cast<GLint>(y * scale_y),
                        static_cast<GLsizei>(width * scale_x),
                        static_cast<GLsizei>(height * scale_y)
                    );
                } else {
                    ::glScissor(x, y, width, height);
                }
            });
        } else {
            ROUTE_REGISTER(router, "glViewport", [](Dynarmic::A32::Jit* cpu) { glViewport(cpu->Regs()[0], cpu->Regs()[1], cpu->Regs()[2], cpu->Regs()[3]); });
            ROUTE_REGISTER(router, "glScissor",  [](Dynarmic::A32::Jit* cpu) { glScissor (cpu->Regs()[0], cpu->Regs()[1], cpu->Regs()[2], cpu->Regs()[3]); });
        }

        ROUTE_REGISTER(router, "glUniform4f", [&memory](Dynarmic::A32::Jit* cpu) {
            GLint location = static_cast<GLint>(cpu->Regs()[0]);

            // ARM softfp calling convention passes floats in integer registers.
            // We read the raw bits and safely copy them into floats.
            uint32_t r1 = cpu->Regs()[1];
            uint32_t r2 = cpu->Regs()[2];
            uint32_t r3 = cpu->Regs()[3];

            // The 5th argument (v3) is passed on the stack
            uint32_t sp = cpu->Regs()[13];
            uint32_t stack_val = memory.Read32(sp);

            float v0, v1, v2, v3;
            std::memcpy(&v0, &r1, sizeof(float));
            std::memcpy(&v1, &r2, sizeof(float));
            std::memcpy(&v2, &r3, sizeof(float));
            std::memcpy(&v3, &stack_val, sizeof(float));

            ::glUniform4f(location, v0, v1, v2, v3);
        });

        ROUTE_REGISTER(router, "glDrawArrays", [](Dynarmic::A32::Jit* cpu) {
            GLenum mode   = static_cast<GLenum>(cpu->Regs()[0]);
            GLint first   = static_cast<GLint>(cpu->Regs()[1]);
            GLsizei count = static_cast<GLsizei>(cpu->Regs()[2]);

            ::glDrawArrays(mode, first, count);
        });

        ROUTE_REGISTER(router, "glDeleteBuffers", [&memory](Dynarmic::A32::Jit* cpu) {
            GLsizei n            = static_cast<GLsizei>(cpu->Regs()[0]);
            uint32_t buffers_ptr = cpu->Regs()[1];

            if (n > 0 && buffers_ptr) {
                const GLuint* buffers = reinterpret_cast<const GLuint*>(memory.GetHostPointer(buffers_ptr));
                ::glDeleteBuffers(n, buffers);
            }
        });
    }
}
