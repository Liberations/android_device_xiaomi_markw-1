/*
 * Copyright (C) 2008 The Android Open Source Project
 * Copyright (C) 2014 The  Linux Foundation. All rights reserved.
 * Copyright (C) 2015 The CyanogenMod Project
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


// #define LOG_NDEBUG 0

#include <cutils/log.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>

#include <sys/ioctl.h>
#include <sys/types.h>

#include <hardware/lights.h>

#ifndef DEFAULT_LOW_PERSISTENCE_MODE_BRIGHTNESS
#define DEFAULT_LOW_PERSISTENCE_MODE_BRIGHTNESS 0x80
#endif

/******************************************************************************/

static pthread_once_t g_init = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct light_state_t g_notification;
static struct light_state_t g_battery;
static int g_last_backlight_mode = BRIGHTNESS_MODE_USER;
static int g_attention = 0;

char const*const RED_LED_FILE
        = "/sys/class/leds/red/brightness";

char const*const GREEN_LED_FILE
        = "/sys/class/leds/green/brightness";

char const*const BLUE_LED_FILE
        = "/sys/class/leds/blue/brightness";

char const*const LCD_FILE
        = "/sys/class/leds/lcd-backlight/brightness";

char const*const LCD_FILE2
        = "/sys/class/backlight/panel0-backlight/brightness";

char const*const BUTTON_FILE
        = "/sys/class/leds/button-backlight/brightness";

char const*const RED_BLINK_FILE
        = "/sys/class/leds/red/blink";

char const*const GREEN_BLINK_FILE
        = "/sys/class/leds/green/blink";

char const*const BLUE_BLINK_FILE
        = "/sys/class/leds/blue/blink";

char const*const PERSISTENCE_FILE
        = "/sys/class/graphics/fb0/msm_fb_persist_mode";

/**
 * device methods
 */

void init_globals(void)
{
    // init the mutex
    pthread_mutex_init(&g_lock, NULL);
}

static int
write_int(char const* path, int value)
{
    int fd;
    static int already_warned = 0;

    fd = open(path, O_RDWR);
    if (fd >= 0) {
        char buffer[20];
        int bytes = snprintf(buffer, sizeof(buffer), "%d\n", value);
        ssize_t amt = write(fd, buffer, (size_t)bytes);
        close(fd);
        return amt == -1 ? -errno : 0;
    } else {
        if (already_warned == 0) {
            ALOGE("write_int failed to open %s\n", path);
            already_warned = 1;
        }
        return -errno;
    }
}

static int
is_lit(struct light_state_t const* state)
{
    return state->color & 0x00ffffff;
}

static int
rgb_to_brightness(struct light_state_t const* state)
{
    int color = state->color & 0x00ffffff;
    return ((77*((color>>16)&0x00ff))
            + (150*((color>>8)&0x00ff)) + (29*(color&0x00ff))) >> 8;
}

static int
set_light_backlight(struct light_device_t* dev,
        struct light_state_t const* state)
{
    int err = 0;
    int brightness = rgb_to_brightness(state);
    unsigned int lpEnabled =
        state->brightnessMode == BRIGHTNESS_MODE_LOW_PERSISTENCE;
    if(!dev) {
        return -1;
    }

    pthread_mutex_lock(&g_lock);
    // Toggle low persistence mode state
    if ((g_last_backlight_mode != state->brightnessMode && lpEnabled) ||
        (!lpEnabled &&
         g_last_backlight_mode == BRIGHTNESS_MODE_LOW_PERSISTENCE)) {
        if ((err = write_int(PERSISTENCE_FILE, lpEnabled)) != 0) {
            ALOGE("%s: Failed to write to %s: %s\n", __FUNCTION__,
                   PERSISTENCE_FILE, strerror(errno));
        }
        if (lpEnabled != 0) {
            brightness = DEFAULT_LOW_PERSISTENCE_MODE_BRIGHTNESS;
        }
    }

    g_last_backlight_mode = state->brightnessMode;

    if (!err) {
        if (!access(LCD_FILE, F_OK)) {
            err = write_int(LCD_FILE, brightness);
        } else {
            err = write_int(LCD_FILE2, brightness);
        }
    }

    pthread_mutex_unlock(&g_lock);
    return err;
}

static int
get_fade_index(int base, int num, int shift_fade)
{
    if (num == 0) return 0; // we can ignore zeroes
    if ((base % num) != 0) return -1; // when values is not divisible
                                      // we cannot synchronize them
    int division = base / num; 

    int max_fade = 6; // 2^6, can be setted to 7
    for (int i = 0; i <= max_fade; i++) {
        if (division == (int)pow(2, i)) // values need to be divided with 2^x
            return i + shift_fade; // shift_fade increases default fade value 
    }

    return -1;
}

static int
match_color_grid_value(int value)
{   // to do: revert array and reimplement for-cycle for it
    const int grid[] = {16, 32, 64, 128, 256}; // we can add 2 more values, but these are enough
    const float diff_rate_great = 0.5f; // round down (64 + 64*0.5 =round_to=> 64) 
    const float diff_rate_less = 0.25f; // round up (64 - 64*0.25 + 1 =round_to=> 64)
    const int   zero_like = 12; // 16 - 16 * 0.25

    if (zero_like >= value) return 0;

    for (int index = 0; index != sizeof(grid)/sizeof(int); index++) { // to do: optimize
        int diff_great = grid[index] * diff_rate_great;
        int diff_less  = grid[index] * diff_rate_less;

        if (grid[index] + diff_great >= value)
            return grid[index];
    }

    // value is more than maximum
    return grid[sizeof(grid)/sizeof(int) - 1]; // the last element
}

static int
min_not_zero(int* values, int size)
{
    int min = values[0]; // can be 0, careful

    for (int i = 0; i != size; i++) {
    	if (values[i] <= 0) continue;
    	if (min <= 0) min = values[i]; // first not null
        if (values[i] < min) 
            min = values[i];
    }

    return min;
}

static void
adapt_colors_for_blink(int* red, int* green, int* blue)
{
    int part[]  = {(*red), (*green), (*blue)}; // color components
    int round[] = {     0,        0,       0}; // rounded to grid
    int rate[]  = {     0,        0,       0}; // minimum component * rate = true component value 
    const int size = sizeof(part)/sizeof(int); // 3
    int max, min, min_round;
    float max_rate, revert_rate;

    // find max element
    max = part[0];
    for (int i = 0; i != size; i++) 
        if (max < part[i]) max = part[i];

    if (max == 0) return; // 0 0 0

    max_rate    =       256.f / (float) max; // max * max_rate = 256
    revert_rate = (float) max / 256.f;       //(max * max_rate) * revert_rate = max

    for (int i = 0; i != size; i++) {
        part[i] = (int)((float)part[i] * max_rate); // max number will be 256,
                                                    // increase elements proportionally
        round[i] = match_color_grid_value(part[i]); // round off values to the grid

        part[i] = round[i] * revert_rate; // return the original scale of values
    }

    min       = min_not_zero(part,  size);
    min_round = min_not_zero(round, size);

    if (min_round > 0) {

        for (int i = 0; i != size; i++) {
            rate[i] = round[i] / min_round; // calculate rates of components

            part[i] = min * rate[i];        // min * rate = true component value 
        }

    }

    // result output
    *red   = part[0];
    *green = part[1];
    *blue  = part[2];
}

static int
set_speaker_light_locked(struct light_device_t* dev,
        struct light_state_t const* state)
{
    int alpha, red, green, blue;
    int max;
    int red, green, blue;
    int blink;
    int onMS, offMS;
    unsigned int colorRGB;
    char breath_pattern_red[16]   = { 0, };
    char breath_pattern_green[16] = { 0, };
    char breath_pattern_blue[16]  = { 0, };
    struct color *nearest = NULL;

    if(!dev) {
        return -1;
    }

    write_int(RED_LED_FILE, 0);
    write_int(GREEN_LED_FILE, 0);
    write_int(BLUE_LED_FILE, 0);

    if (state == NULL) {
        return 0;
    }

    switch (state->flashMode) {
        case LIGHT_FLASH_TIMED:
            onMS = state->flashOnMS;
            offMS = state->flashOffMS;
            break;
        case LIGHT_FLASH_NONE:
        default:
            onMS = 0;
            offMS = 0;
            break;
    }

    colorRGB = state->color;

#if 0
    ALOGD("set_speaker_light_locked mode %d, colorRGB=%08X, onMS=%d, offMS=%d\n",
            state->flashMode, colorRGB, onMS, offMS);
#endif

    red = (colorRGB >> 16) & 0xFF;
    green = (colorRGB >> 8) & 0xFF;
    blue = colorRGB & 0xFF;

    if (onMS > 0 && offMS > 0) {
        /*
         * if ON time == OFF time
         *   use blink mode 2
         * else
         *   use blink mode 1
         */
        if (onMS == offMS)
            blink = 2;
        else
            blink = 1;
    } else {
        blink = 0;
    }

    if (blink) {
        if (red) {
            if (write_int(RED_BLINK_FILE, blink))
                write_int(RED_LED_FILE, 0);
	}
        if (green) {
            if (write_int(GREEN_BLINK_FILE, blink))
                write_int(GREEN_LED_FILE, 0);
	}
        if (blue) {
            if (write_int(BLUE_BLINK_FILE, blink))
                write_int(BLUE_LED_FILE, 0);
	}
    } else {
        write_int(RED_LED_FILE, red);
        write_int(GREEN_LED_FILE, green);
        write_int(BLUE_LED_FILE, blue);
    }

    return 0;
}

static void
handle_speaker_battery_locked(struct light_device_t* dev)
{
    if (is_lit(&g_battery)) {
        set_speaker_light_locked(dev, &g_battery);
    } else {
        set_speaker_light_locked(dev, &g_notification);
    }
}

static int
set_light_battery(struct light_device_t* dev,
        struct light_state_t const* state)
{
    pthread_mutex_lock(&g_lock);
    g_battery = *state;
    handle_speaker_battery_locked(dev);
    pthread_mutex_unlock(&g_lock);
    return 0;
}

static int
set_light_notifications(struct light_device_t* dev,
        struct light_state_t const* state)
{
    pthread_mutex_lock(&g_lock);
    g_notification = *state;
    handle_speaker_battery_locked(dev);
    pthread_mutex_unlock(&g_lock);
    return 0;
}

static int
set_light_attention(struct light_device_t* dev,
        struct light_state_t const* state)
{
    pthread_mutex_lock(&g_lock);
    if (state->flashMode == LIGHT_FLASH_HARDWARE) {
        g_attention = state->flashOnMS;
    } else if (state->flashMode == LIGHT_FLASH_NONE) {
        g_attention = 0;
    }
    handle_speaker_battery_locked(dev);
    pthread_mutex_unlock(&g_lock);
    return 0;
}

static int
set_light_buttons(struct light_device_t* dev,
        struct light_state_t const* state)
{
    int err = 0;
    if(!dev) {
        return -1;
    }
    pthread_mutex_lock(&g_lock);
    err = write_int(BUTTON_FILE, state->color & 0xFF);
    pthread_mutex_unlock(&g_lock);
    return err;
}

/** Close the lights device */
static int
close_lights(struct light_device_t *dev)
{
    if (dev) {
        free(dev);
    }
    return 0;
}


/******************************************************************************/

/**
 * module methods
 */

/** Open a new instance of a lights device using name */
static int open_lights(const struct hw_module_t* module, char const* name,
        struct hw_device_t** device)
{
    int (*set_light)(struct light_device_t* dev,
            struct light_state_t const* state);

    if (0 == strcmp(LIGHT_ID_BACKLIGHT, name))
        set_light = set_light_backlight;
    else if (0 == strcmp(LIGHT_ID_BATTERY, name))
        set_light = set_light_battery;
    else if (0 == strcmp(LIGHT_ID_NOTIFICATIONS, name))
        set_light = set_light_notifications;
    else if (0 == strcmp(LIGHT_ID_BUTTONS, name))
        set_light = set_light_buttons;
    else if (0 == strcmp(LIGHT_ID_ATTENTION, name))
        set_light = set_light_attention;
    else
        return -EINVAL;

    pthread_once(&g_init, init_globals);

    struct light_device_t *dev = malloc(sizeof(struct light_device_t));

    if(!dev)
        return -ENOMEM;

    memset(dev, 0, sizeof(*dev));

    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = LIGHTS_DEVICE_API_VERSION_2_0;
    dev->common.module = (struct hw_module_t*)module;
    dev->common.close = (int (*)(struct hw_device_t*))close_lights;
    dev->set_light = set_light;

    *device = (struct hw_device_t*)dev;
    return 0;
}

static struct hw_module_methods_t lights_module_methods = {
    .open =  open_lights,
};

/*
 * The lights Module
 */
struct hw_module_t HAL_MODULE_INFO_SYM = {
    .tag = HARDWARE_MODULE_TAG,
    .version_major = 1,
    .version_minor = 0,
    .id = LIGHTS_HARDWARE_MODULE_ID,
    .name = "lights Module",
    .author = "Google, Inc.",
    .methods = &lights_module_methods,
};
