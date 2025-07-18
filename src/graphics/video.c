#include "video.h"

#include "core/config.h"
#include "core/dir.h"
#include "core/file.h"
#include "core/smacker.h"
#include "core/time.h"
#include "game/campaign.h"
#include "game/system.h"
#include "graphics/renderer.h"
#include "platform/file_manager.h"
#include "sound/device.h"
#include "sound/music.h"
#include "sound/speech.h"

#include "easyav1.h"
#include "pl_mpeg/pl_mpeg.h"

#include <string.h>

#define MAX_FRAME_TIME_ADVANCE_MS (1.0 / 30.0)

typedef enum {
    VIDEO_TYPE_NONE = 0,
    VIDEO_TYPE_SMK = 1,
    VIDEO_TYPE_MPG = 2,
    VIDEO_TYPE_AV1 = 3
} video_type;

static struct {
    int is_playing;
    int is_ended;

    smacker s;
    plm_t *plm;
    easyav1_t *easyav1;

    video_type type;

    struct {
        int width;
        int height;
        int y_scale;
        int micros_per_frame;
        time_millis start_render_millis;
        int current_frame;
        int draw_frame;
        plm_frame_t *mpg_frame;
    } video;
    struct {
        int has_audio;
        int bitdepth;
        int channels;
        int rate;
    } audio;
    struct {
        color_t *pixels;
        int width;
    } buffer;
    int restart_music;
} data;

static void close_decoder(void)
{
    if (data.s) {
        smacker_close(data.s);
        data.s = 0;
    }
    if (data.plm) {
        plm_destroy(data.plm);
        data.plm = 0;
        data.type = VIDEO_TYPE_NONE;
    }
    if (data.easyav1) {
        easyav1_stop(data.easyav1);
        easyav1_destroy(&data.easyav1);
        data.type = VIDEO_TYPE_NONE;
    }
    data.type = VIDEO_TYPE_NONE;
}

static void update_mpg_video(plm_t *plm, plm_frame_t *frame, void *user)
{
    data.video.draw_frame = 1;
    data.video.current_frame++;
    data.video.mpg_frame = frame;
}

static void update_mpg_audio(plm_t *mpeg, plm_samples_t *samples, void *user)
{
    sound_device_write_custom_music_data(samples->interleaved, sizeof(float) * samples->count * 2);
}

static const char *get_filename_from_path(const char *filename)
{
    if (data.type == VIDEO_TYPE_SMK) {
        return filename;
    } else if (data.type == VIDEO_TYPE_AV1) {
        return filename;
    } else if (data.type == VIDEO_TYPE_MPG) {
        return filename;
    }
    return NULL;
}

static int load_av1(const char *filename)
{
    if (data.type == VIDEO_TYPE_SMK || data.type == VIDEO_TYPE_MPG) {
        return 0;
    }
    char av1_filename[FILE_NAME_MAX];
    int position = 0;

    // Copy filename and change "smk/" to "av1/"
    if (strncmp(filename, "smk/", 4) == 0) {
        position = snprintf(av1_filename, FILE_NAME_MAX, "av1/%s", file_remove_path(filename));
    } else if (strncmp(filename, "smk\\", 4) == 0) {
        position = snprintf(av1_filename, FILE_NAME_MAX, "av1\\%s", file_remove_path(filename));
    } else {
        position = snprintf(av1_filename, FILE_NAME_MAX, "%s", filename);
    }

    if (position + 5 >= FILE_NAME_MAX) {
        return 0; // Not enough space for the new extension
    }

    // We cannot use file_change_extension here because it can only replace 3 characters
    filename = strrchr(av1_filename, '.');
    if (filename) {
        position = filename - av1_filename; // Get the position of the last dot
    }

    snprintf(av1_filename + position, FILE_NAME_MAX - position, ".webm");

    data.easyav1 = 0;
    size_t length;
    uint8_t *video_buffer = game_campaign_load_file(av1_filename, &length);
    if (video_buffer) {
        data.easyav1 = easyav1_init_from_memory(video_buffer, length, 0);
    }
    if (!data.easyav1) {
        const char *path;
        const char *community_location = platform_file_manager_get_directory_for_location(PATH_LOCATION_COMMUNITY, 0);
        if (strncmp(community_location, av1_filename, strlen(community_location)) == 0) {
            path = filename;
        } else {
            path = dir_get_file(av1_filename, MAY_BE_LOCALIZED);
        }
        if (!path) {
            return 0;
        }
        FILE *av1 = file_open(path, "rb");
        if (!av1) {
            return 0;
        }
        easyav1_settings settings = easyav1_default_settings();
        settings.close_handle_on_destroy = EASYAV1_TRUE;

        if (!config_get(CONFIG_GENERAL_ENABLE_VIDEO_SOUND)) {
            settings.enable_audio = EASYAV1_FALSE;
        }
        data.easyav1 = easyav1_init_from_file(av1, &settings);
    }

    if (!data.easyav1) {
        return 0;
    }
    data.video.width = easyav1_get_video_width(data.easyav1);
    data.video.height =  easyav1_get_video_height(data.easyav1);
    data.video.y_scale = SMACKER_Y_SCALE_NONE;
    data.video.current_frame = 0;
    data.video.micros_per_frame = 1000000 / easyav1_get_video_fps(data.easyav1);

    data.audio.has_audio = 0;

    if (easyav1_has_audio_track(data.easyav1)) {
        data.audio.has_audio = 1;
        data.audio.bitdepth = 32;
        data.audio.channels = easyav1_get_audio_channels(data.easyav1);
        data.audio.rate = easyav1_get_audio_sample_rate(data.easyav1);
    }

    data.type = VIDEO_TYPE_AV1;
    return 1;
}

static int load_mpg(const char *filename)
{
    if (data.type == VIDEO_TYPE_SMK || data.type == VIDEO_TYPE_AV1) {
        return 0;
    }
    char mpg_filename[FILE_NAME_MAX];
    snprintf(mpg_filename, FILE_NAME_MAX, "%s", filename);
    file_change_extension(mpg_filename, "mpg");
    if (strncmp(mpg_filename, "smk/", 4) == 0 || strncmp(mpg_filename, "smk\\", 4) == 0) {
        mpg_filename[0] = 'm';
        mpg_filename[1] = 'p';
        mpg_filename[2] = 'g';
    }
    data.plm = 0;
    size_t length;
    uint8_t *video_buffer = game_campaign_load_file(mpg_filename, &length);
    if (video_buffer) {
        data.plm = plm_create_with_memory(video_buffer, length, 1);
    }
    if (!data.plm) {
        const char *path;
        const char *community_location = platform_file_manager_get_directory_for_location(PATH_LOCATION_COMMUNITY, 0);
        if (strncmp(community_location, mpg_filename, strlen(community_location)) == 0) {
            path = filename;
        } else {
            path = dir_get_file(mpg_filename, MAY_BE_LOCALIZED);
        }
        if (!path) {
            return 0;
        }
        FILE *mpg = file_open(path, "rb");
        data.plm = plm_create_with_file(mpg, 1);
    }

    if (!data.plm) {
        return 0;
    }
    data.video.width = plm_get_width(data.plm);
    data.video.height = plm_get_height(data.plm);
    data.video.y_scale = SMACKER_Y_SCALE_NONE;
    data.video.current_frame = 0;
    data.video.micros_per_frame = (int) (1000000 / plm_get_framerate(data.plm));

    data.audio.has_audio = 0;

	plm_set_video_decode_callback(data.plm, update_mpg_video, 0);
	
    if (config_get(CONFIG_GENERAL_ENABLE_VIDEO_SOUND) && plm_get_num_audio_streams(data.plm) > 0) {
        plm_set_audio_enabled(data.plm, 1);
        plm_set_audio_stream(data.plm, 0);
        data.audio.has_audio = 1;
        data.audio.bitdepth = 32;
        data.audio.channels = 2;
        data.audio.rate = plm_get_samplerate(data.plm);
        plm_set_audio_decode_callback(data.plm, update_mpg_audio, 0);
    } else {
        plm_set_audio_enabled(data.plm, 0);
        plm_set_audio_decode_callback(data.plm, 0, 0);
    }

    data.type = VIDEO_TYPE_MPG;
    return 1;
}

static int load_smk(const char *filename)
{
    if (data.type == VIDEO_TYPE_MPG || data.type == VIDEO_TYPE_AV1) {
        return 0;
    }
    const char *path = dir_get_file(filename, MAY_BE_LOCALIZED);
    if (!path) {
        return 0;
    }
    FILE *fp = file_open(path, "rb");
    data.s = smacker_open(fp);
    if (!data.s) {
        // smacker_open() closes the stream on error: no need to close fp
        return 0;
    }

    int width, height, y_scale, micros_per_frame;
    smacker_get_frames_info(data.s, 0, &micros_per_frame);
    smacker_get_video_info(data.s, &width, &height, &y_scale);

    data.video.width = width;
    data.video.height = y_scale == SMACKER_Y_SCALE_NONE ? height : height * 2;
    data.video.y_scale = y_scale;
    data.video.current_frame = 0;
    data.video.micros_per_frame = micros_per_frame;

    data.audio.has_audio = 0;
    if (config_get(CONFIG_GENERAL_ENABLE_VIDEO_SOUND)) {
        int has_track, channels, bitdepth, rate;
        smacker_get_audio_info(data.s, 0, &has_track, &channels, &bitdepth, &rate);
        if (has_track) {
            data.audio.has_audio = 1;
            data.audio.bitdepth = bitdepth;
            data.audio.channels = channels;
            data.audio.rate = rate;
        }
    }

    if (smacker_first_frame(data.s) != SMACKER_FRAME_OK) {
        close_decoder();
        return 0;
    }
    data.type = VIDEO_TYPE_SMK;
    return 1;
}

static void end_video(void)
{
    sound_device_use_default_music_player();
    if (data.restart_music) {
        sound_music_update(1);
    }
    graphics_renderer()->release_custom_image_buffer(CUSTOM_IMAGE_VIDEO);
}

int video_start(const char *filename)
{
    data.is_playing = 0;
    data.is_ended = 0;

    if (load_av1(filename) || load_mpg(filename) || load_smk(filename)) {
        sound_music_pause();
        sound_speech_stop();
        int is_yuv = data.type != VIDEO_TYPE_SMK && graphics_renderer()->supports_yuv_image_format();
        graphics_renderer()->create_custom_image(CUSTOM_IMAGE_VIDEO, data.video.width, data.video.height, is_yuv);
        if (!is_yuv) {
            data.buffer.pixels = graphics_renderer()->get_custom_image_buffer(CUSTOM_IMAGE_VIDEO, &data.buffer.width);
        }
        if (data.type == VIDEO_TYPE_AV1) {
            easyav1_play(data.easyav1);
        }
        data.is_playing = 1;
        return 1;
    } else {
        data.type = VIDEO_TYPE_NONE;
        return 0;
    }
}

void video_size(int *width, int *height)
{
    *width = data.video.width;
    *height = data.video.y_scale == SMACKER_Y_SCALE_NONE ? data.video.height : 2 * data.video.height;
}

void video_init(int restart_music)
{
    data.video.start_render_millis = system_get_ticks() - 1;
    data.restart_music = restart_music;

    if (data.audio.has_audio) {
        int audio_len;
        const void *audio_data;
        if (data.type == VIDEO_TYPE_SMK) {
            audio_len = smacker_get_frame_audio_size(data.s, 0);
            audio_data = smacker_get_frame_audio(data.s, 0);
        } else {
            audio_len = 0;
            audio_data = 0;
        }
        if (data.audio.has_audio) {
            sound_device_use_custom_music_player(data.audio.bitdepth, data.audio.channels, data.audio.rate,
                audio_data, audio_len);
        }
    }
}

int video_is_finished(void)
{
    return data.is_ended;
}

void video_stop(void)
{
    if (data.is_playing) {
        if (!data.is_ended) {
            end_video();
        }
        close_decoder();
        data.is_playing = 0;
    }
}

void video_shutdown(void)
{
    if (data.is_playing) {
        close_decoder();
        data.is_playing = 0;
    }
}

static void get_next_frame(void)
{
    if (data.type == VIDEO_TYPE_NONE || (data.type == VIDEO_TYPE_SMK && !data.s) ||
        (data.type == VIDEO_TYPE_MPG && !data.plm) ||
        (data.type == VIDEO_TYPE_AV1 && !data.easyav1)) {
        return;
    }
    time_millis now_millis = system_get_ticks();

    if (data.type == VIDEO_TYPE_SMK) {
        int frame_no = (now_millis - data.video.start_render_millis) * 1000 / data.video.micros_per_frame;
        data.video.draw_frame = data.video.current_frame == 0;
        while (frame_no > data.video.current_frame) {
            if (smacker_next_frame(data.s) != SMACKER_FRAME_OK) {
                close_decoder();
                data.is_ended = 1;
                data.is_playing = 0;
                end_video();
                return;
            }
            data.video.current_frame++;
            data.video.draw_frame = 1;

            if (data.audio.has_audio) {
                int audio_len = smacker_get_frame_audio_size(data.s, 0);
                const void *audio_data = smacker_get_frame_audio(data.s, 0);
                if (audio_len > 0) {
                    sound_device_write_custom_music_data(audio_data, audio_len);
                }
            }
        }
    } else if (data.type == VIDEO_TYPE_MPG) {
        double elapsed_time = (now_millis - data.video.start_render_millis) / 1000.0;
        if (elapsed_time > MAX_FRAME_TIME_ADVANCE_MS) {
            elapsed_time = MAX_FRAME_TIME_ADVANCE_MS;
        }
        plm_decode(data.plm, elapsed_time);
        data.video.start_render_millis = now_millis;
        if (plm_has_ended(data.plm)) {
            close_decoder();
            data.is_ended = 1;
            data.is_playing = 0;
            end_video();
        }
    } else if (data.type == VIDEO_TYPE_AV1) {
        if (data.audio.has_audio) {
            const easyav1_audio_frame *audio_frame = easyav1_get_audio_frame(data.easyav1);
            if (audio_frame) {
                sound_device_write_custom_music_data(audio_frame->pcm.interlaced, (int) audio_frame->bytes);
            }
        }

        if (easyav1_has_video_frame(data.easyav1)) {
            data.video.draw_frame = 1;
            data.video.current_frame++;
        } else {
            data.video.draw_frame = 0;
        }

        if (easyav1_is_finished(data.easyav1)) {
            close_decoder();
            data.is_ended = 1;
            data.is_playing = 0;
            end_video();
        }
    }
}

static void update_video_frame(void)
{
    if (data.type == VIDEO_TYPE_NONE) {
        return;
    }
    if (data.type == VIDEO_TYPE_SMK) {
        const unsigned char *frame = smacker_get_frame_video(data.s);
        const uint32_t *pal = smacker_get_frame_palette(data.s);
        if (frame && pal) {
            for (int y = 0; y < data.video.height; y++) {
                color_t *pixel = &data.buffer.pixels[y * data.buffer.width];
                int video_y = data.video.y_scale == SMACKER_Y_SCALE_NONE ? y : y / 2;
                const unsigned char *line = frame + (video_y * data.video.width);
                for (int x = 0; x < data.video.width; x++) {
                    *pixel = ALPHA_OPAQUE | pal[line[x]];
                    ++pixel;
                }
            }
        }
    } else if (data.type == VIDEO_TYPE_MPG) {
        if (graphics_renderer()->supports_yuv_image_format()) {
            plm_frame_t *frame = data.video.mpg_frame;
            graphics_renderer()->update_custom_image_yuv(CUSTOM_IMAGE_VIDEO, frame->y.data, frame->y.width,
                frame->cb.data, frame->cb.width, frame->cr.data, frame->cr.width);
            return;
        }
        plm_frame_to_bgra(data.video.mpg_frame, (uint8_t *) data.buffer.pixels, data.buffer.width * 4);
    } else if (data.type == VIDEO_TYPE_AV1) {
        if (!data.easyav1) {
            return;
        }
        const easyav1_video_frame *frame = easyav1_get_video_frame(data.easyav1);
        if (!frame || !graphics_renderer()->supports_yuv_image_format()) {
            return;
        }
        graphics_renderer()->update_custom_image_yuv(CUSTOM_IMAGE_VIDEO, frame->data[0], (int) frame->stride[0],
            frame->data[1], (int) frame->stride[1], frame->data[2], (int) frame->stride[2]);
        return;
    }
    graphics_renderer()->update_custom_image(CUSTOM_IMAGE_VIDEO);
}

void video_draw(int x_offset, int y_offset, int width, int height)
{
    get_next_frame();
    if (data.video.draw_frame) {
        update_video_frame();
        data.video.draw_frame = 0;
    }

    float scale = 1.0f;

    if (data.video.width != width || data.video.height != height) {
        float scale_w = data.video.width / (float) width;
        float scale_h = data.video.height / (float) height;
        scale = scale_w > scale_h ? scale_w : scale_h;

        if (scale == scale_h) {
            x_offset += (int) ((width - data.video.width / scale) / 2 * scale);
        }
        if (scale == scale_w) {
            y_offset += (int) ((height - data.video.height / scale) / 2 * scale);
        }
    }
    // Only draw the video if it has frames to show, otherwise it may display garbage
    if (data.video.current_frame != 0) {
        graphics_renderer()->draw_custom_image(CUSTOM_IMAGE_VIDEO, x_offset, y_offset, scale, 0);
    }
}
