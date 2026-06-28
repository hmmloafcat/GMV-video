#ifndef GMV_PLAYER_H
#define GMV_PLAYER_H

#include <GLFW/glfw3.h>
#include <stdbool.h>

typedef struct {
    // Media Metadata
    unsigned int width;
    unsigned int height;
    float fps;
    unsigned int frame_count;
    unsigned int audio_size;
    
    // Playback State & Controls
    bool paused;
    bool stopped;
    bool loop;
    float speed;
    float brightness;
    float x, y;
    float scale_x, scale_y;
    float rotation;       // Rotation angle in degrees
    float volume;         // Volume range: 0.0 to 100.0
    bool flip_x;
    bool flip_y;
    float colorFilter[3]; // RGB multipliers [0.0 - 1.0+]
    
    // Internals
    float current_time; 
    unsigned int last_frame_index;
    GLuint texture_id;
    unsigned char* video_data_buffer;
    size_t* frame_offsets;
    size_t* frame_sizes;
    
    // OpenAL Audio Fields
    unsigned int al_source;
    unsigned int al_buffer;
    bool has_audio;
} GmvPlayer;

// Public API
GmvPlayer* gmv_open(const char* filename);
void gmv_update(GmvPlayer* player, float delta_time);
void gmv_render(GmvPlayer* player);
void gmv_close(GmvPlayer* player);
void gmv_seek(GmvPlayer* player, float time_in_seconds);

#endif // GMV_PLAYER_H

// ============================================================================
// IMPLEMENTATION
// ============================================================================
#ifdef GMV_PLAYER_IMPLEMENTATION

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// STB Image & Vorbis Setup
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "stb_vorbis.c"

// OpenAL Headers (Adjust paths based on OS if needed, e.g., <OpenAL/al.h> on macOS)
#include <AL/al.h>
#include <AL/alc.h>

// Simple JPEG analyzer to calculate byte lengths of sequential frames instantly
static void _parse_jpeg_frames(GmvPlayer* player, unsigned char* start_ptr, size_t total_size) {
    player->frame_offsets = malloc(sizeof(size_t) * player->frame_count);
    player->frame_sizes = malloc(sizeof(size_t) * player->frame_count);
    
    size_t offset = 0;
    unsigned int f_idx = 0;
    
    while (offset < total_size - 1 && f_idx < player->frame_count) {
        if (start_ptr[offset] == 0xFF && start_ptr[offset + 1] == 0xD8) {
            player->frame_offsets[f_idx] = offset;
            if (f_idx > 0) {
                player->frame_sizes[f_idx - 1] = offset - player->frame_offsets[f_idx - 1];
            }
            f_idx++;
        }
        offset++;
    }
    if (f_idx > 0) {
        player->frame_sizes[f_idx - 1] = total_size - player->frame_offsets[f_idx - 1];
    }
}

GmvPlayer* gmv_open(const char* filename) {
    FILE* f = fopen(filename, "rb");
    if (!f) {
        fprintf(stderr, "[GMV] Failed to open file: %s\n", filename);
        return NULL;
    }

    char magic[4];
    fread(magic, 1, 4, f);
    if (memcmp(magic, "GMV1", 4) != 0) {
        fprintf(stderr, "[GMV] Invalid file magic format.\n");
        fclose(f);
        return NULL;
    }

    GmvPlayer* player = calloc(1, sizeof(GmvPlayer));
    fread(&player->width, 4, 1, f);
    fread(&player->height, 4, 1, f);
    fread(&player->fps, 4, 1, f);
    fread(&player->frame_count, 4, 1, f);
    fread(&player->audio_size, 4, 1, f);

    // Default configuration values
    player->paused = false;
    player->stopped = false;
    player->loop = false;
    player->speed = 1.0f;
    player->brightness = 1.0f;
    player->scale_x = 1.0f;
    player->scale_y = 1.0f;
    player->rotation = 0.0f;
    player->volume = 100.0f;
    player->flip_x = false;
    player->flip_y = false;
    player->colorFilter[0] = 1.0f;
    player->colorFilter[1] = 1.0f;
    player->colorFilter[2] = 1.0f;
    player->last_frame_index = 0xFFFFFFFF; // Force update first frame

    // 1. Process Audio Stream (OpenAL & stb_vorbis)
    if (player->audio_size > 0) {
        unsigned char* audio_buf = malloc(player->audio_size);
        fread(audio_buf, 1, player->audio_size, f);
        
        int channels, sample_rate;
        short* output_samples = NULL;
        int num_samples = stb_vorbis_decode_memory(audio_buf, player->audio_size, &channels, &sample_rate, &output_samples);
        
        if (num_samples > 0) {
            alGenBuffers(1, &player->al_buffer);
            alGenSources(1, &player->al_source);
            
            ALenum format = (channels == 2) ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16;
            alBufferData(player->al_buffer, format, output_samples, num_samples * channels * sizeof(short), sample_rate);
            alSourcei(player->al_source, AL_BUFFER, player->al_buffer);
            
            player->has_audio = true;
            free(output_samples);
        }
        free(audio_buf);
    }

    // 2. Process Video Stream
    long video_start_pos = ftell(f);
    fseek(f, 0, SEEK_END);
    long total_video_bytes = ftell(f) - video_start_pos;
    fseek(f, video_start_pos, SEEK_SET);

    player->video_data_buffer = malloc(total_video_bytes);
    fread(player->video_data_buffer, 1, total_video_bytes, f);
    fclose(f);

    _parse_jpeg_frames(player, player->video_data_buffer, total_video_bytes);

    // Setup OpenGL Container Texture
    glGenTextures(1, &player->texture_id);
    glBindTexture(GL_TEXTURE_2D, player->texture_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    if (player->has_audio && !player->paused) {
        alSourcePlay(player->al_source);
    }

    return player;
}

void gmv_update(GmvPlayer* player, float delta_time) {
    if (!player) return;

    // Handle full stop overrides
    if (player->stopped) {
        if (player->has_audio) {
            alSourceStop(player->al_source);
        }
        player->current_time = 0.0f;
        player->last_frame_index = 0xFFFFFFFF; // Force refresh to frame 0
        return;
    }

    if (player->paused) {
        if (player->has_audio) alSourcePause(player->al_source);
        return;
    }

    // Synchronize or manual scale ticks via delta_time
    if (player->has_audio) {
        ALint state;
        alGetSourcei(player->al_source, AL_SOURCE_STATE, &state);
        
        // Dynamically match user properties (Volume slider downscale and pitch processing)
        float target_gain = player->volume / 100.0f;
        if (target_gain < 0.0f) target_gain = 0.0f;
        if (target_gain > 1.0f) target_gain = 1.0f;
        alSourcef(player->al_source, AL_GAIN, target_gain);
        alSourcef(player->al_source, AL_PITCH, player->speed);

        if (state == AL_PLAYING) {
            float al_offset;
            alGetSourcef(player->al_source, AL_SEC_OFFSET, &al_offset);
            player->current_time = al_offset;
        } else if (state == AL_PAUSED) {
            // Resume if it was mistakenly paused outside state flags
            alSourcePlay(player->al_source);
        } else {
            // Audio finished processing
            if (player->loop) {
                gmv_seek(player, 0.0f);
            } else {
                player->paused = true;
                player->current_time = (float)player->frame_count / player->fps;
            }
        }
    } else {
        player->current_time += delta_time * player->speed;
        float max_duration = (float)player->frame_count / player->fps;
        if (player->current_time >= max_duration) {
            if (player->loop) {
                player->current_time = 0.0f;
            } else {
                player->current_time = max_duration;
                player->paused = true;
            }
        }
    }

    // Compute active target frame
    unsigned int frame_index = (unsigned int)(player->current_time * player->fps);
    if (frame_index >= player->frame_count) frame_index = player->frame_count - 1;

    // Only swap texture data when crossing into a new index marker
    if (frame_index != player->last_frame_index) {
        player->last_frame_index = frame_index;
        
        unsigned char* jpg_ptr = player->video_data_buffer + player->frame_offsets[frame_index];
        size_t jpg_sz = player->frame_sizes[frame_index];
        
        int w, h, comp;
        unsigned char* pixels = stbi_load_from_memory(jpg_ptr, (int)jpg_sz, &w, &h, &comp, 4);
        if (pixels) {
            glBindTexture(GL_TEXTURE_2D, player->texture_id);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
            stbi_image_free(pixels);
        }
    }
}

void gmv_render(GmvPlayer* player) {
    if (!player || player->texture_id == 0) return;

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, player->texture_id);

    // Apply filters, tint combinations, and explicit brightness matrices
    glColor4f(
        player->colorFilter[0] * player->brightness,
        player->colorFilter[1] * player->brightness,
        player->colorFilter[2] * player->brightness,
        1.0f
    );

    // Handle Transform Matrix Stack
    glPushMatrix();
    
    // 1. Move origin to position coordinate
    glTranslatef(player->x, player->y, 0.0f);
    
    // 2. Pivot orientation rotation and structural scale vectors
    glRotatef(player->rotation, 0.0f, 0.0f, 1.0f);
    glScalef(player->scale_x, player->scale_y, 1.0f);

    // Compute uv mapping bounds matching flipped state preferences
    float u_min = player->flip_x ? 1.0f : 0.0f;
    float u_max = player->flip_x ? 0.0f : 1.0f;
    float v_min = player->flip_y ? 1.0f : 0.0f;
    float v_max = player->flip_y ? 0.0f : 1.0f;

    glBegin(GL_QUADS);
        glTexCoord2f(u_min, v_min); glVertex2f(0.0f, 0.0f);
        glTexCoord2f(u_max, v_min); glVertex2f((float)player->width, 0.0f);
        glTexCoord2f(u_max, v_max); glVertex2f((float)player->width, (float)player->height);
        glTexCoord2f(u_min, v_max); glVertex2f(0.0f, (float)player->height);
    glEnd();

    glPopMatrix();

    glDisable(GL_TEXTURE_2D);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f); // Reset Context State
}

void gmv_seek(GmvPlayer* player, float time_in_seconds) {
    if (!player) return;
    float max_duration = (float)player->frame_count / player->fps;
    if (time_in_seconds < 0.0f) time_in_seconds = 0.0f;
    if (time_in_seconds > max_duration) time_in_seconds = max_duration;

    player->current_time = time_in_seconds;
    player->last_frame_index = 0xFFFFFFFF; // Reset texture check flags

    if (player->has_audio) {
        alSourcef(player->al_source, AL_SEC_OFFSET, time_in_seconds);
        if (!player->paused && !player->stopped) {
            alSourcePlay(player->al_source);
        }
    }
}

void gmv_close(GmvPlayer* player) {
    if (!player) return;
    if (player->has_audio) {
        alSourceStop(player->al_source);
        alDeleteSources(1, &player->al_source);
        alDeleteBuffers(1, &player->al_buffer);
    }
    if (player->texture_id) {
        glDeleteTextures(1, &player->texture_id);
    }
    free(player->video_data_buffer);
    free(player->frame_offsets);
    free(player->frame_sizes);
    free(player);
}

#endif // GMV_PLAYER_IMPLEMENTATION
