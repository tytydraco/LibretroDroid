/*
 * Copyright (C) 2009 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// OpenGL ES 2.0 code

#include <jni.h>
#include <android/log.h>

#include <GLES3/gl3.h>
#include <GLES3/gl3ext.h>
#include <EGL/egl.h>

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <dlfcn.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <vector>
#include <cmath>

#include <oboe/Oboe.h>

extern "C" {
#include "utils.h"
#include "libretro.h"
}

#define  LOG_TAG    "libgl2jni"
#define  LOGD(...)  __android_log_print(ANDROID_LOG_DEBUG,LOG_TAG,__VA_ARGS__)
#define  LOGI(...)  __android_log_print(ANDROID_LOG_INFO,LOG_TAG,__VA_ARGS__)
#define  LOGE(...)  __android_log_print(ANDROID_LOG_ERROR,LOG_TAG,__VA_ARGS__)

static void printGLString(const char *name, GLenum s) {
    const char *v = (const char *) glGetString(s);
    LOGI("GL %s = %s\n", name, v);
}

static void checkGlError(const char* op) {
    for (GLint error = glGetError(); error; error
            = glGetError()) {
        LOGE("after %s() glError (0x%x)\n", op, error);
    }
}

const char* gVertexShader =
"attribute vec4 vPosition;\n"
"attribute vec2 vCoordinate;\n"
"uniform lowp sampler2D texture;\n"
"varying vec2 coords;\n"
"void main() {\n"
"  coords = vCoordinate;\n"
"  gl_Position = vPosition;\n"
"}\n";

const char* gFragmentShader =
"precision mediump float;\n"
"uniform lowp sampler2D texture;\n"
"varying vec2 coords;\n"
"void main() {\n"
"  vec4 tex = texture2D(texture, coords);"
"  gl_FragColor = vec4(tex.rgb, 1.0);\n"
"}\n";

GLuint loadShader(GLenum shaderType, const char* pSource) {
    GLuint shader = glCreateShader(shaderType);
    if (shader) {
        glShaderSource(shader, 1, &pSource, nullptr);
        glCompileShader(shader);
        GLint compiled = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        if (!compiled) {
            GLint infoLen = 0;
            glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &infoLen);
            if (infoLen) {
                char* buf = (char*) malloc(infoLen);
                if (buf) {
                    glGetShaderInfoLog(shader, infoLen, nullptr, buf);
                    LOGE("Could not compile shader %d:\n%s\n",
                            shaderType, buf);
                    free(buf);
                }
                glDeleteShader(shader);
                shader = 0;
            }
        }
    }
    return shader;
}

GLuint createProgram(const char* pVertexSource, const char* pFragmentSource) {
    GLuint vertexShader = loadShader(GL_VERTEX_SHADER, pVertexSource);
    if (!vertexShader) {
        return 0;
    }

    GLuint pixelShader = loadShader(GL_FRAGMENT_SHADER, pFragmentSource);
    if (!pixelShader) {
        return 0;
    }

    GLuint program = glCreateProgram();
    if (program) {
        glAttachShader(program, vertexShader);
        checkGlError("glAttachShader");
        glAttachShader(program, pixelShader);
        checkGlError("glAttachShader");
        glLinkProgram(program);
        GLint linkStatus = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &linkStatus);
        if (linkStatus != GL_TRUE) {
            GLint bufLength = 0;
            glGetProgramiv(program, GL_INFO_LOG_LENGTH, &bufLength);
            if (bufLength) {
                char* buf = (char*) malloc(bufLength);
                if (buf) {
                    glGetProgramInfoLog(program, bufLength, nullptr, buf);
                    LOGE("Could not link program:\n%s\n", buf);
                    free(buf);
                }
            }
            glDeleteProgram(program);
            program = 0;
        }
    }
    return program;
}

GLuint gProgram;
GLuint gvPositionHandle;
GLuint gvCoordinateHandle;

int width = 0;
int height = 0;

GLuint textureHandle;
GLuint tex;

GLfloat gTriangleVertices[] = {
        -1.0f, -1.0f,
        -1.0f,1.0f,
        1.0f, -1.0f,
        1.0f, -1.0f,
        -1.0f, 1.0f,
        1.0f, 1.0f
};

const GLfloat gTriangleCoords[] = {
        0.0f, 0.0f,
        0.0f, 1.0f,
        1.0f, 0.0f,
        1.0f, 0.0f,
        0.0f, 1.0f,
        1.0f, 1.0f
};

oboe::AudioStream* stream = nullptr;

void initializeAudio(int32_t sampleRate) {
    oboe::AudioStreamBuilder builder;
    builder.setChannelCount(2);
    builder.setSampleRate(sampleRate);
    builder.setDirection(oboe::Direction::Output);
    builder.setFormat(oboe::AudioFormat::I16);

    oboe::Result result = builder.openStream(&stream);
    if (result != oboe::Result::OK) {
        LOGE("Failed to create stream. Error: %s", oboe::convertToText(result));
    }

    stream->requestStart();
}

bool setupGraphics(int screenWidth, int screenHeight, int gameWidth, int gameHeight) {
    printGLString("Version", GL_VERSION);
    printGLString("Vendor", GL_VENDOR);
    printGLString("Renderer", GL_RENDERER);
    printGLString("Extensions", GL_EXTENSIONS);

    width = screenWidth;
    height = screenHeight;

    LOGI("setupGraphics(%d, %d)", screenWidth, screenHeight);
    gProgram = createProgram(gVertexShader, gFragmentShader);
    if (!gProgram) {
        LOGE("Could not create program.");
        return false;
    }

    gvPositionHandle = glGetAttribLocation(gProgram, "vPosition");
    checkGlError("glGetAttribLocation");
    LOGI("glGetAttribLocation(\"vPosition\") = %d\n", gvPositionHandle);

    gvCoordinateHandle = glGetAttribLocation(gProgram, "vCoordinate");
    checkGlError("glGetAttribLocation");
    LOGI("glGetAttribLocation(\"vCoordinate\") = %d\n", gvCoordinateHandle);

    // Initialize texture
    // FILIPPO
    //create test checker image
    textureHandle = glGetUniformLocation(gProgram, "texture");
    checkGlError("glGetAttribLocation");
    LOGI("glGetUniformLocation(\"texture\") = %d\n", textureHandle);

    glViewport(0, 0, screenWidth, screenHeight);
    checkGlError("glViewport");
    return true;
}

unsigned current_framebuffer = 0;
unsigned current_framebuffer_texture = 0;

void renderFrame() {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    glViewport(0, 0, width, height);

    /*glDisable(GL_DEPTH_TEST);*/
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glUseProgram(gProgram);
    checkGlError("glUseProgram");

    glDisable(GL_DEPTH_TEST);

    glVertexAttribPointer(gvPositionHandle, 2, GL_FLOAT, GL_FALSE, 0, gTriangleVertices);
    checkGlError("glVertexAttribPointer");
    glEnableVertexAttribArray(gvPositionHandle);
    checkGlError("glEnableVertexAttribArray");

    glVertexAttribPointer(gvCoordinateHandle, 2, GL_FLOAT, GL_FALSE, 0, gTriangleCoords);
    checkGlError("glVertexAttribPointer");
    glEnableVertexAttribArray(gvCoordinateHandle);
    checkGlError("glEnableVertexAttribArray");

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, current_framebuffer_texture);
    glUniform1i(textureHandle, 0);

    glDrawArrays(GL_TRIANGLES, 0, 6);
    checkGlError("glDrawArrays");

    glBindTexture(GL_TEXTURE_2D, 0);
    checkGlError("glBindTexture");

    glUseProgram(0);
}

void* get_symbol(void* handle, const char* symbol) {
    void* result = dlsym(handle, symbol);
    if (!result) {
        LOGE("Cannot get symbol %s... Quitting", symbol);
        exit(1);
    }
    return result;
}

// Retrograde callbacks

void hw_initialize_framebuffer(unsigned width, unsigned height) {
    bool depth = true;
    bool stencil = false;

    glGenFramebuffers(1, &current_framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, current_framebuffer);

    glGenTextures(1, &current_framebuffer_texture);
    glBindTexture(GL_TEXTURE_2D, current_framebuffer_texture);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, width, height);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, current_framebuffer_texture, 0);

    if (depth) {
        unsigned int depth_buffer;
        glGenRenderbuffers(1, &depth_buffer);
        glBindRenderbuffer(GL_RENDERBUFFER, depth_buffer);
        glRenderbufferStorage(GL_RENDERBUFFER, stencil ? GL_DEPTH24_STENCIL8 : GL_DEPTH_COMPONENT16, width, height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, stencil? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth_buffer);
    }

    if(glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        LOGE("Error while create framebuffer. Leaving!");
        exit(2);
    }

    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
}

void callback_retro_log(enum retro_log_level level, const char *fmt, ...) {
    va_list argptr;
    va_start(argptr, fmt);

    if (level < RETRO_LOG_DEBUG) {
        LOGD(fmt, argptr);
    } else if (level < RETRO_LOG_ERROR) {
        LOGI(fmt, argptr);
    } else {
        LOGE(fmt, argptr);
    }

    va_end(argptr);
}

void callback_hw_video_refresh(const void *data, unsigned width, unsigned height, size_t pitch) {
    LOGI("hw video refresh callback called %i %i", width, height);
    if (data == RETRO_HW_FRAME_BUFFER_VALID) {
        //renderFrame();
        //renderFrame();
    }
}

std::vector<struct retro_variable> variables;

retro_hw_context_reset_t hw_context_reset = nullptr;
retro_hw_context_reset_t hw_context_destroy = nullptr;

bool environment_handle_set_variables(const struct retro_variable* received) {
    unsigned count = 0;
    while (received[count].key != nullptr) {
        LOGI("%s: %s", received[count].key, received[count].value);
        count++;
    }

    variables.clear();

    for (int i = 0; i < count; i++) {
        char* key = (char*) malloc(sizeof(char) * strlen(received[i].key));
        strcpy(key, received[i].key);

        char* start = nullptr;
        char* end = nullptr;
        start = (char*) strchr(received[i].value, ';') + 2;
        end = (char*) strchr(received[i].value, '|');

        char* value;
        if (start != nullptr && end != nullptr) {
            value = (char*) calloc(sizeof(char), (end - start + 1));
            memcpy(value, start, end - start);
        } else {
            value = (char*) malloc(sizeof(char) * strlen(received[i].value));
            strcpy(key, received[i].value);
        }

        struct retro_variable variable = { key, value };

        LOGI("Adding parsed variable: %s %s", key, value);

        variables.push_back(variable);
    }

    return true;
}

bool environment_handle_get_variable(struct retro_variable* requested) {
    LOGI("%s", requested->key);

    for (int i = 0; i < variables.size(); i++) {
        if (strcmp(variables[i].key, requested->key) == 0) {
            requested->value = variables[i].value;
            return true;
        }
    }

    return false;
}

uintptr_t hw_get_current_framebuffer() {
    LOGI("FILIPPO... Calling hw_get_current_framebuffer %u", current_framebuffer);
    return current_framebuffer;
}

bool environment_handle_set_hw_render(struct retro_hw_render_callback* hw_render_callback) {
    LOGI("FILIPPO... Calling environment_handle_set_hw_render");
    hw_context_destroy = hw_render_callback->context_destroy;
    hw_context_reset = hw_render_callback->context_reset;
    hw_render_callback->get_current_framebuffer = &hw_get_current_framebuffer;
    hw_render_callback->get_proc_address = &eglGetProcAddress;

    return true;
}

bool callback_environment(unsigned cmd, void *data) {
    switch (cmd) {
        case RETRO_ENVIRONMENT_GET_CAN_DUPE:
            *((bool*) data) = true;
            return true;

        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
            *(const char**) data = "/storage/emulated/0/test-system";
            return true;

        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
            LOGI("Called SET_PIXEL_FORMAT");
            return true;

        case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
            LOGI("Callsed SET_INPUT_DESCRIPTORS");
            return false;

        case RETRO_ENVIRONMENT_GET_VARIABLE:
            LOGI("Called RETRO_ENVIRONMENT_GET_VARIABLE");
            return environment_handle_get_variable(static_cast<struct retro_variable*>(data));

        case RETRO_ENVIRONMENT_SET_VARIABLES:
            LOGI("Called RETRO_ENVIRONMENT_SET_VARIABLES");
            return environment_handle_set_variables(static_cast<const struct retro_variable*>(data));

        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
            LOGI("Called RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE");
            return false;

        case RETRO_ENVIRONMENT_SET_HW_RENDER:
            LOGI("Called RETRO_ENVIRONMENT_SET_HW_RENDER");
            return environment_handle_set_hw_render(static_cast<struct retro_hw_render_callback*>(data));

        case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE:
            LOGI("Called RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE");
            return false;

        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
            LOGI("Called RETRO_ENVIRONMENT_GET_LOG_INTERFACE");
            ((struct retro_log_callback*) data)->log = &callback_retro_log;
            return true;

        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
            LOGI("Called RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY");
            *(const char**) data = "/storage/emulated/0/test-saves";
            return false;

        case RETRO_ENVIRONMENT_GET_PERF_INTERFACE:
            LOGI("Called RETRO_ENVIRONMENT_GET_PERF_INTERFACE");
            return false;

        case RETRO_ENVIRONMENT_SET_GEOMETRY:
            LOGI("Called RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO");
            return false;

        case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
            LOGI("Called RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE");
            return false;
    }

    LOGI("callback environment has been called: %u", cmd);
    return false;
}

void callback_audio_sample(int16_t left, int16_t right) {
    //LOGI("callback audio sample has been called");
}

size_t callback_set_audio_sample_batch(const int16_t *data, size_t frames) {
    if (stream != nullptr) {
        stream->write(data, frames, 0);
    }
    //LOGI("callback audio sample has been called");
    return frames;
}

void callback_retro_set_input_poll() {
    LOGI("callback_retro_set_input_poll has been called");
}

int16_t callback_set_input_state(unsigned port, unsigned device, unsigned index, unsigned id) {
    LOGI("callback_set_input_state has been called");
    return 0;
}

// INITIALIZATION

void (*w_retro_init)(void);
void (*w_retro_deinit)(void);
unsigned (*w_retro_api_version)(void);
void (*w_retro_get_system_info)(struct retro_system_info *info);
void (*w_retro_get_system_av_info)(struct retro_system_av_info *info);
void (*w_retro_set_controller_port_device)(unsigned port, unsigned device);
void (*w_retro_reset)(void);
void (*w_retro_run)(void);
size_t (*w_retro_serialize_size)(void);
bool (*w_retro_serialize)(void *data, size_t size);
bool (*w_retro_unserialize)(const void *data, size_t size);
bool (*w_retro_load_game)(const struct retro_game_info *game);
void (*w_retro_unload_game)(void);
void (*w_retro_set_video_refresh)(retro_video_refresh_t);
void (*w_retro_set_environment)(retro_environment_t);
void (*w_retro_set_audio_sample)(retro_audio_sample_t);
void (*w_retro_set_audio_sample_batch)(retro_audio_sample_batch_t);
void (*w_retro_set_input_poll)(retro_input_poll_t);
void (*w_retro_set_input_state)(retro_input_state_t);


extern "C" {
    JNIEXPORT void JNICALL Java_com_android_gl2jni_GL2JNILib_init(JNIEnv * env, jobject obj,  jint width, jint height);
    JNIEXPORT void JNICALL Java_com_android_gl2jni_GL2JNILib_step(JNIEnv * env, jobject obj);
};

JNIEXPORT void JNICALL Java_com_android_gl2jni_GL2JNILib_init(JNIEnv * env, jobject obj,  jint width, jint height)
{
    //void* handle = dlopen("pcsx_rearmed_libretro_android.so", RTLD_LOCAL);
    void* handle = dlopen("mupen64plus_next_gles3_libretro_android.so", RTLD_LOCAL);
    //void* handle = dlopen("libretro-test-gl.so", RTLD_LOCAL);

    if (!handle) {
        LOGE("Cannot dlopen library, closing");
        exit(1);
    }

    // ATTACH RETRO API

    w_retro_init = (void (*)()) get_symbol(handle, "retro_init");
    w_retro_deinit = (void (*)()) get_symbol(handle, "retro_deinit");
    w_retro_api_version = (unsigned (*)()) get_symbol(handle, "retro_api_version");
    w_retro_get_system_info = (void (*)(struct retro_system_info*)) get_symbol(handle, "retro_get_system_info");
    w_retro_get_system_av_info = (void (*)(struct retro_system_av_info*)) get_symbol(handle, "retro_get_system_av_info");
    w_retro_set_controller_port_device = (void (*)(unsigned, unsigned)) get_symbol(handle, "retro_set_controller_port_device");
    w_retro_reset = (void (*)()) get_symbol(handle, "retro_reset");
    w_retro_run = (void (*)()) get_symbol(handle, "retro_run");
    w_retro_serialize_size = (size_t (*)()) get_symbol(handle, "retro_serialize_size");
    w_retro_serialize = (bool (*)(void*, size_t)) get_symbol(handle, "retro_serialize");
    w_retro_unserialize = (bool (*)(const void*, size_t)) get_symbol(handle, "retro_unserialize");
    w_retro_load_game = (bool (*)(const struct retro_game_info*)) get_symbol(handle, "retro_load_game");
    w_retro_unload_game = (void (*)()) get_symbol(handle, "retro_unload_game");

    // CALLBACK MANAGEMENT

    w_retro_set_video_refresh = (void (*)(retro_video_refresh_t)) get_symbol(handle, "retro_set_video_refresh");
    w_retro_set_environment = (void (*)(retro_environment_t)) get_symbol(handle, "retro_set_environment");
    w_retro_set_audio_sample = (void (*)(retro_audio_sample_t)) get_symbol(handle, "retro_set_audio_sample");
    w_retro_set_audio_sample_batch = (void (*)(retro_audio_sample_batch_t)) get_symbol(handle, "retro_set_audio_sample_batch");
    w_retro_set_input_poll = (void (*)(retro_input_poll_t)) get_symbol(handle, "retro_set_input_poll");
    w_retro_set_input_state = (void (*)(retro_input_state_t)) get_symbol(handle, "retro_set_input_state");

    // Attach callbacks
    w_retro_set_video_refresh(&callback_hw_video_refresh);
    w_retro_set_environment(&callback_environment);
    w_retro_set_audio_sample(&callback_audio_sample);
    w_retro_set_audio_sample_batch(&callback_set_audio_sample_batch);
    w_retro_set_input_poll(&callback_retro_set_input_poll);
    w_retro_set_input_state(&callback_set_input_state);

    w_retro_init();

    struct retro_system_info system_info;
    w_retro_get_system_info(&system_info);

    struct retro_game_info game_info;
    if (system_info.need_fullpath) {
        game_info.path = "/storage/emulated/0/Roms Test/n64/Super Mario 64/Super Mario 64.n64";
        //game_info->path = "/storage/emulated/0/Roms/psx/Metal Gear Solid.pbp";
        game_info.data = nullptr;
        game_info.size = 0;
    } else {
        struct read_file_result file = read_file_as_bytes("/storage/emulated/0/Roms Test/n64/Super Mario 64/Super Mario 64.n64");
        //struct read_file_result file = read_file_as_bytes("/storage/emulated/0/Roms/psx/Metal Gear Solid.pbp");
        game_info.data = file.data;
        game_info.size = file.size;
    }

    bool result = w_retro_load_game(&game_info);
    if (!result) {
        LOGI("Cannot load game. Leaving.");
        exit(1);
    }

    struct retro_system_av_info system_av_info;
    w_retro_get_system_av_info(&system_av_info);

    initializeAudio(std::lround(system_av_info.timing.sample_rate));

    setupGraphics(width, height, system_av_info.geometry.base_width, system_av_info.geometry.base_width);
    hw_initialize_framebuffer(system_av_info.geometry.base_width, system_av_info.geometry.base_height);

    if (hw_context_reset != nullptr) {
        hw_context_reset();
    }
}


JNIEXPORT void JNICALL Java_com_android_gl2jni_GL2JNILib_step(JNIEnv * env, jobject obj)
{
    //time_t begin = clock();
    LOGI("Begin of retro_run");
    w_retro_run();
    renderFrame();
    LOGI("End of retro_run");
    //LOGI("Rendered frame %f", 1000 * ((double) current - (double) last_frame) / CLOCKS_PER_SEC);
}


