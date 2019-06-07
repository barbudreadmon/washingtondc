// This is early WIP
// TODO : 
// - implement sound and inputs
// - use libretro's glsm instead of glew in backend
// - write Makefile (or maybe we'll use CMake ?)
// - implement other runtime settings as core options
// - whatever i mention as TODO in this file
// - whatever i forgot

#include <unistd.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>

#ifdef _MSC_VER
#define snprintf _snprintf
#endif

#include <sys/stat.h>

#include "washdc/washdc.h"
#include "washdc/buildconfig.h"

#include <libretro.h>

#include <file/file_path.h>

static struct washdc_launch_settings settings = { };

static char slash = path_default_slash_c();

static char g_save_dir[PATH_MAX];
static char g_system_dir[PATH_MAX];
static char path_gdi[PATH_MAX];
static char bios_path[PATH_MAX];
static char flash_path[PATH_MAX];

static int game_width;
static int game_height;

static int pad_type[4] = {1,1,1,1};
static bool all_devices_ready = false;

struct retro_perf_callback perf_cb;
retro_get_cpu_features_t perf_get_cpu_features_cb = NULL;

static retro_log_printf_t log_cb;
static retro_video_refresh_t video_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_environment_t environ_cb;
static retro_audio_sample_batch_t audio_batch_cb;

extern struct retro_hw_render_callback hw_render;

static bool direct_boot = false;

void retro_set_environment(retro_environment_t cb)
{
    static const struct retro_variable vars[] = {
        {"washingtondc_direct_boot", "Enable direct boot (skip BIOS); disabled|enabled" },
        { NULL, NULL },
    };

    static const struct retro_controller_description peripherals[] = {
        { "Gamepad", RETRO_DEVICE_JOYPAD },
        { "Keyboard", RETRO_DEVICE_KEYBOARD },
        { "None", RETRO_DEVICE_NONE },
    };

    static const struct retro_controller_info ports[] = {
        { peripherals, 3 },
        { peripherals, 3 },
        { peripherals, 3 },
        { peripherals, 3 },
        { NULL, 0 },
    };

    environ_cb = cb;

    cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)vars);
    environ_cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)ports);
}
void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

static int first_ctx_reset = 1;

// TODO : Use this to get the right fbo value in backend (opengl_target.cpp ?)
int libretro_get_current_framebuffer(void)
{
    return hw_render.get_current_framebuffer();
}

static void context_reset(void)
{
    glsm_ctl(GLSM_CTL_STATE_CONTEXT_RESET, NULL);
    glsm_ctl(GLSM_CTL_STATE_SETUP, NULL);
    if (first_ctx_reset == 1)
    {
        first_ctx_reset = 0;
        washdc_init(&settings);
    }
    else
    {
        // TODO : Only start rendering
    }
}

static void context_destroy(void)
{
    // TODO : Only stop rendering
    glsm_ctl(GLSM_CTL_STATE_CONTEXT_DESTROY, NULL);
}

static bool init_hardware_context(void)
{
    // TODO : change values to request the right context (request glcore 4.5 for now)
    glsm_ctx_params_t params = {0};
    params.context_reset = context_reset;
    params.context_destroy = context_destroy;
    params.environ_cb = environ_cb;
    params.stencil = true;
    params.context_type = RETRO_HW_CONTEXT_OPENGL_CORE;
    params.major = 4;
    params.minor = 5;
    if (!glsm_ctl(GLSM_CTL_STATE_CONTEXT_INIT, &params))
        return false;
    return true;
}

static struct retro_system_av_info g_av_info;

void retro_get_system_info(struct retro_system_info *info)
{
    memset(info, 0, sizeof(*info));
    info->library_name     = "WashingtonDC";
#ifndef GIT_VERSION
#define GIT_VERSION ""
#endif
    info->library_version  = "v0" GIT_VERSION;
    info->need_fullpath    = true;
    info->valid_extensions = "gdi";
}

void check_variables(void)
{
    struct retro_variable var;

    var.key = "washingtondc_direct_boot";
    var.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        if (strcmp(var.value, "disabled") == 0)
            direct_boot = false;
        else if (strcmp(var.value, "enabled") == 0)
            direct_boot = true;
    }
}

static void set_descriptors(void)
{
    struct retro_input_descriptor *input_descriptors = (struct retro_input_descriptor*)calloc(22*4+1, sizeof(struct retro_input_descriptor));

    unsigned j = 0;
    for (unsigned i = 0; i < 4; i++)
    {
        switch (pad_type[i])
        {
            case RETRO_DEVICE_JOYPAD:
                input_descriptors[j++] = (struct retro_input_descriptor){ i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" };
                input_descriptors[j++] = (struct retro_input_descriptor){ i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" };
                input_descriptors[j++] = (struct retro_input_descriptor){ i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" };
                input_descriptors[j++] = (struct retro_input_descriptor){ i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" };
                input_descriptors[j++] = (struct retro_input_descriptor){ i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "A" };
                input_descriptors[j++] = (struct retro_input_descriptor){ i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "B" };
                input_descriptors[j++] = (struct retro_input_descriptor){ i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "X" };
                input_descriptors[j++] = (struct retro_input_descriptor){ i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Y" };
                input_descriptors[j++] = (struct retro_input_descriptor){ i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "L Trigger" };
                input_descriptors[j++] = (struct retro_input_descriptor){ i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "R Trigger" };
                input_descriptors[j++] = (struct retro_input_descriptor){ i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start" };
                input_descriptors[j++] = (struct retro_input_descriptor){ i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X,  "Analog X" };
                input_descriptors[j++] = (struct retro_input_descriptor){ i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y,  "Analog Y" };
                break;
            case RETRO_DEVICE_KEYBOARD:
                // TODO : implement keyboard
                break;
        }
    }
    input_descriptors[j].description = NULL;
    environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, input_descriptors);
    free(input_descriptors);
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
    memset(info, 0, sizeof(*info));

    info->timing.fps            = (retro_get_region() == RETRO_REGION_NTSC) ? 60.0f : 50.0f;
    info->timing.sample_rate    = 44100.0;
    info->geometry.base_width   = game_width;
    info->geometry.base_height  = game_height;
    info->geometry.max_width    = game_width;
    info->geometry.max_height   = game_width;
    info->geometry.aspect_ratio = 4.0 / 3.0;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
    if(pad_type[port] != device)
    {
        pad_type[port] = device;
        // When all devices are set, we can send input descriptors
        if (all_devices_ready)
            set_descriptors();
    }
}

size_t retro_serialize_size(void)
{
   return 0;
}

bool retro_serialize(void *data, size_t size)
{
   return true;
}

bool retro_unserialize(const void *data, size_t size)
{
   return true;
}

void retro_cheat_reset(void)
{
}

void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
}

void retro_init(void)
{
    struct retro_log_callback log;
    const char *dir = NULL;

    log_cb                   = NULL;
    perf_get_cpu_features_cb = NULL;
    uint64_t serialization_quirks = RETRO_SERIALIZATION_QUIRK_SINGLE_SESSION; 
    unsigned level           = 16;

    if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
        log_cb = log.log;

    if (environ_cb(RETRO_ENVIRONMENT_GET_PERF_INTERFACE, &perf_cb))
        perf_get_cpu_features_cb = perf_cb.get_cpu_features;

    if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) && dir)
        strncpy(g_system_dir, dir, sizeof(g_system_dir));

    if (environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) && dir)
        strncpy(g_save_dir, dir, sizeof(g_save_dir));

    char save_dir[PATH_MAX];
    snprintf(save_dir, sizeof(save_dir), "%s%cwashingtondc%c", g_save_dir, slash, slash);
    path_mkdir(save_dir);

    environ_cb(RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL, &level);

    environ_cb(RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS, &serialization_quirks);
}

static int does_file_exist(const char *filename)
{
    struct stat st;
    int result = stat(filename, &st);
    return result == 0;
}

bool retro_load_game(const struct retro_game_info *info)
{
    if (!info)
        return false;

    check_variables();

    snprintf(path_gdi, sizeof(path_gdi), "%s", info->path);

    snprintf(bios_path, sizeof(bios_path), "%s%cwashingtondc%cdc_boot.bin", g_system_dir, slash, slash);
    if (does_file_exist(bios_path) != 1)
    {
        log_cb(RETRO_LOG_WARN, "%s NOT FOUND\n", bios_path);
        snprintf(bios_path, sizeof(bios_path), "%s%cdc%cdc_boot.bin", g_system_dir, slash, slash);
        if (does_file_exist(bios_path) != 1)
        {
            log_cb(RETRO_LOG_WARN, "%s NOT FOUND\n", bios_path);
        }
    }

    snprintf(flash_path, sizeof(flash_path), "%s%cwashingtondc%cdc_flash.bin", g_system_dir, slash, slash);
    if (does_file_exist(flash_path) != 1)
    {
        log_cb(RETRO_LOG_WARN, "%s NOT FOUND\n", flash_path);
        snprintf(bios_path, sizeof(flash_path), "%s%cdc%cdc_flash.bin", g_system_dir, slash, slash);
        if (does_file_exist(flash_path) != 1)
        {
            log_cb(RETRO_LOG_WARN, "%s NOT FOUND\n", flash_path);
        }
    }

    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
        return false;
    if (!init_hardware_context())
        return false;

    settings.path_dc_bios = bios_path;
    settings.path_dc_flash = flash_path;
    settings.path_gdi = path_gdi;

    return true;
}

bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t num_info)
{
    return false;
}

void retro_unload_game(void)
{
    // TODO : close content
}

unsigned retro_get_region(void)
{
    return RETRO_REGION_NTSC;
}

unsigned retro_api_version(void)
{
    return RETRO_API_VERSION;
}

void *retro_get_memory_data(unsigned id)
{
    return NULL;
}

size_t retro_get_memory_size(unsigned id)
{
    return 0;
}

void retro_deinit(void)
{
    // TODO : close emu
}

void retro_reset(void)
{
    // TODO : reset game
}

void retro_run(void)
{
    bool updated  = false;
    if (!all_devices_ready)
    {
        // Running first frame, so we can assume all devices id were set
        // Let's send input descriptors
        all_devices_ready = true;
        set_descriptors();
    }

    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
    {
        check_variables();
        // TODO : Change settings
    }

    washdc_run_one_frame();
}
