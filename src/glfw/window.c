/*******************************************************************************
 *
 *
 *    WashingtonDC Dreamcast Emulator
 *    Copyright (C) 2017-2019 snickerbockers
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *
 ******************************************************************************/

#include <err.h>
#include <ctype.h>
#include <stdbool.h>

#include <GLFW/glfw3.h>

#include "gfx/opengl/opengl_output.h"
#include "dreamcast.h"
#include "hw/maple/maple_controller.h"
#include "gfx/gfx.h"
#include "title.h"
#include "config_file.h"

#include "window.h"

#define TOGGLE_OVERLAY_KEY GLFW_KEY_F2

static unsigned res_x, res_y;
static GLFWwindow *win;

static void expose_callback(GLFWwindow *win);
static void resize_callback(GLFWwindow *win, int width, int height);
static void scan_input(void);

void win_init(unsigned width, unsigned height) {
    res_x = width;
    res_y = height;

    if (!glfwInit())
        err(1, "unable to initialize glfw");

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_RESIZABLE, GL_TRUE);
    glfwWindowHint(GLFW_DEPTH_BITS, 24);

    win = glfwCreateWindow(res_x, res_y, title_get(), NULL, NULL);

    if (!win)
        errx(1, "unable to create window");

    glfwSetWindowRefreshCallback(win, expose_callback);
    glfwSetFramebufferSizeCallback(win, resize_callback);

    bool vsync_en = false;
    if (cfg_get_bool("win.vsync", &vsync_en) == 0 && vsync_en) {
        LOG_INFO("vsync enabled\n");
        glfwSwapInterval(1);
    } else {
        LOG_INFO("vsync disabled\n");
        glfwSwapInterval(0);
    }
}

void win_cleanup() {
    glfwTerminate();
}

void win_check_events(void) {
    glfwPollEvents();

    scan_input();

    if (glfwWindowShouldClose(win))
        dreamcast_kill();
}

void win_update() {
    glfwSwapBuffers(win);
}

static void expose_callback(GLFWwindow *win) {
    gfx_expose();
}

enum gamepad_btn {
    GAMEPAD_BTN_A = 0,
    GAMEPAD_BTN_B = 1,
    GAMEPAD_BTN_X = 2,
    GAMEPAD_BTN_Y = 3,
    GAMEPAD_BTN_START = 7,

    GAMEPAD_BTN_COUNT
};

enum gamepad_hat {
    GAMEPAD_HAT_UP,
    GAMEPAD_HAT_DOWN,
    GAMEPAD_HAT_LEFT,
    GAMEPAD_HAT_RIGHT,

    GAMEPAD_HAT_COUNT
};

static void scan_input(void) {
    int btn_cnt, axis_cnt;
    const unsigned char *gamepad_state =
        glfwGetJoystickButtons(GLFW_JOYSTICK_1, &btn_cnt);
    const float *axis_state = glfwGetJoystickAxes(GLFW_JOYSTICK_1, &axis_cnt);

    bool btns[GAMEPAD_BTN_COUNT] = { 0 };
    bool hat[GAMEPAD_HAT_COUNT] = { 0 };

    /*
     * TODO: these controller bindings are based on my Logitech F510.  They will
     * be different for other controllers, so there needs to be a custom binding
     * system
     */
    if (gamepad_state && btn_cnt >= GAMEPAD_BTN_COUNT) {
        btns[GAMEPAD_BTN_A] = gamepad_state[GAMEPAD_BTN_A];
        btns[GAMEPAD_BTN_B] = gamepad_state[GAMEPAD_BTN_B];
        btns[GAMEPAD_BTN_X] = gamepad_state[GAMEPAD_BTN_X];
        btns[GAMEPAD_BTN_Y] = gamepad_state[GAMEPAD_BTN_Y];
        btns[GAMEPAD_BTN_START] = gamepad_state[GAMEPAD_BTN_START];
    }

    // neutral position
    unsigned stick_hor = 128;
    unsigned stick_vert = 128;
    unsigned hat_hor = 128;
    unsigned hat_vert = 128;
    unsigned trig_l = 0, trig_r = 0;

    if (axis_cnt >= 1)
        stick_hor = (unsigned)(axis_state[0] * 128) + 128;

    if (axis_cnt >= 2)
        stick_vert = (unsigned)(axis_state[1] * 128) + 128;

    if (axis_cnt >= 7)
        hat_hor = (unsigned)(axis_state[6] * 128) + 128;

    if (axis_cnt >= 8)
        hat_vert = (unsigned)(axis_state[7] * 128) + 128;

    if (axis_cnt >= 3)
        trig_l = (unsigned)(axis_state[2] * 128) + 128;

    if (axis_cnt >= 6)
        trig_r = (unsigned)(axis_state[5] * 128) + 128;

    if (stick_hor > 255)
        stick_hor = 255;
    if (stick_vert > 255)
        stick_vert = 255;
    if (hat_hor > 255)
        hat_hor = 255;
    if (hat_vert > 255)
        hat_vert = 255;
    if (trig_l > 255)
        trig_l = 255;
    if (trig_r > 255)
        trig_r = 255;

    if (hat_vert <= 64)
        hat[GAMEPAD_HAT_UP] = true;
    else if (hat_vert >= 192)
        hat[GAMEPAD_HAT_DOWN] = true;

    if (hat_hor <= 64)
        hat[GAMEPAD_HAT_LEFT] = true;
    else if (hat_hor >= 192)
        hat[GAMEPAD_HAT_RIGHT] = true;

    btns[GAMEPAD_BTN_A] = btns[GAMEPAD_BTN_A] ||
        (glfwGetKey(win, GLFW_KEY_KP_2) == GLFW_PRESS);
    btns[GAMEPAD_BTN_B] = btns[GAMEPAD_BTN_B] ||
        (glfwGetKey(win, GLFW_KEY_KP_6) == GLFW_PRESS);
    btns[GAMEPAD_BTN_X] = btns[GAMEPAD_BTN_X] ||
        (glfwGetKey(win, GLFW_KEY_KP_4) == GLFW_PRESS);
    btns[GAMEPAD_BTN_Y] = btns[GAMEPAD_BTN_Y] ||
        (glfwGetKey(win, GLFW_KEY_KP_8) == GLFW_PRESS);
    btns[GAMEPAD_BTN_START] = btns[GAMEPAD_BTN_START] ||
        (glfwGetKey(win, GLFW_KEY_SPACE) == GLFW_PRESS);

    hat[GAMEPAD_HAT_UP] = hat[GAMEPAD_HAT_UP] ||
        (glfwGetKey(win, GLFW_KEY_W) == GLFW_PRESS);
    hat[GAMEPAD_HAT_DOWN] = hat[GAMEPAD_HAT_DOWN] ||
        (glfwGetKey(win, GLFW_KEY_S) == GLFW_PRESS);
    hat[GAMEPAD_HAT_LEFT] = hat[GAMEPAD_HAT_LEFT] ||
        (glfwGetKey(win, GLFW_KEY_A) == GLFW_PRESS);
    hat[GAMEPAD_HAT_RIGHT] = hat[GAMEPAD_HAT_RIGHT] ||
        (glfwGetKey(win, GLFW_KEY_D) == GLFW_PRESS);

    if (btns[GAMEPAD_BTN_A])
        maple_controller_press_btns(0, MAPLE_CONT_BTN_A_MASK);
    else
        maple_controller_release_btns(0, MAPLE_CONT_BTN_A_MASK);
    if (btns[GAMEPAD_BTN_B])
        maple_controller_press_btns(0, MAPLE_CONT_BTN_B_MASK);
    else
        maple_controller_release_btns(0, MAPLE_CONT_BTN_B_MASK);
    if (btns[GAMEPAD_BTN_X])
        maple_controller_press_btns(0, MAPLE_CONT_BTN_X_MASK);
    else
        maple_controller_release_btns(0, MAPLE_CONT_BTN_X_MASK);
    if (btns[GAMEPAD_BTN_Y])
        maple_controller_press_btns(0, MAPLE_CONT_BTN_Y_MASK);
    else
        maple_controller_release_btns(0, MAPLE_CONT_BTN_Y_MASK);
    if (btns[GAMEPAD_BTN_START])
        maple_controller_press_btns(0, MAPLE_CONT_BTN_START_MASK);
    else
        maple_controller_release_btns(0, MAPLE_CONT_BTN_START_MASK);

    if (hat[GAMEPAD_HAT_UP])
        maple_controller_press_btns(0, MAPLE_CONT_BTN_DPAD_UP_MASK);
    else
        maple_controller_release_btns(0, MAPLE_CONT_BTN_DPAD_UP_MASK);
    if (hat[GAMEPAD_HAT_DOWN])
        maple_controller_press_btns(0, MAPLE_CONT_BTN_DPAD_DOWN_MASK);
    else
        maple_controller_release_btns(0, MAPLE_CONT_BTN_DPAD_DOWN_MASK);
    if (hat[GAMEPAD_HAT_LEFT])
        maple_controller_press_btns(0, MAPLE_CONT_BTN_DPAD_LEFT_MASK);
    else
        maple_controller_release_btns(0, MAPLE_CONT_BTN_DPAD_LEFT_MASK);
    if (hat[GAMEPAD_HAT_RIGHT])
        maple_controller_press_btns(0, MAPLE_CONT_BTN_DPAD_RIGHT_MASK);
    else
        maple_controller_release_btns(0, MAPLE_CONT_BTN_DPAD_RIGHT_MASK);

    maple_controller_set_axis(0, MAPLE_CONTROLLER_AXIS_R_TRIG, trig_r);
    maple_controller_set_axis(0, MAPLE_CONTROLLER_AXIS_L_TRIG, trig_l);
    maple_controller_set_axis(0, MAPLE_CONTROLLER_AXIS_JOY1_X, stick_hor);
    maple_controller_set_axis(0, MAPLE_CONTROLLER_AXIS_JOY1_Y, stick_vert);
    maple_controller_set_axis(0, MAPLE_CONTROLLER_AXIS_JOY2_X, 0);
    maple_controller_set_axis(0, MAPLE_CONTROLLER_AXIS_JOY2_Y, 0);

    // Allow the user to toggle the overlay by pressing F2
    static bool overlay_key_prev = false;
    bool overlay_key = glfwGetKey(win, TOGGLE_OVERLAY_KEY);
    if (overlay_key && !overlay_key_prev)
        dc_toggle_overlay();
    overlay_key_prev = overlay_key;
}

void win_make_context_current(void) {
    glfwMakeContextCurrent(win);
}

void win_update_title(void) {
    glfwSetWindowTitle(win, title_get());
}

static void resize_callback(GLFWwindow *win, int width, int height) {
    res_x = width;
    res_y = height;
    gfx_resize(width, height);
}

int win_get_width(void) {
    return res_x;
}

int win_get_height(void) {
    return res_y;
}
