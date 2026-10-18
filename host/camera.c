/* The orbit camera: the mouse turns the game's camera round the character (camera.h).
 *
 * FFXiMain's scene (the global FFXI_MODERN_DISP_SCENE, 0x104568fc in 2026-09-03) holds its camera at
 * +0x50: the eye at +0x44 and the point it looks at at +0x50 (x, height, z; the height grows
 * downward). In third person the game's own camera code moves both every frame from where they are:
 * the look-at point follows the character's head and the eye trails it, turned by the arrow keys
 * round the look-at point, kept 3 to 6 units out (eased back in from farther) and out of the ground.
 * So an eye put somewhere stays there, and the game carries on from it: a drag turns the eye round
 * the look-at point, and a zoom out of the game's range is put back every frame. In first person the
 * game sets the look-at point from its own aim every frame; the mouse is left to the game there.
 *
 * The mouse: a press is kept from the game until it moves a few points (a drag: the cursor is hidden
 * and held, and the motion turns the camera) or comes up (a click: the press is handed to the game
 * with its release, which is what the game acts on). The game's own drag (either button) turns the
 * camera too, but slowly and by raising the eye, not round the character; it never sees a drag now.
 * Its windows don't move with the mouse, so a drag over them turns the camera as well. Both buttons
 * held run forward (the numpad's 8, which moves the character away from the camera). The wheel zooms
 * over the world; over the interface (where it was drawn last frame, user32_ui_hit) it scrolls the
 * chat log as before. Cutscenes are left alone: the camera is only touched while the player is not in
 * an event and the look-at point is at the character. */
#include "camera.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <string.h>

#include "runtime.h"
#include "input.h"
#include "user32.h"
#include "gfx.h"
#include "addons/game.h"
#include "addons/xi.h"
#include "build.h"

#define DRAG_POINTS 4.0f  /* how far a press moves before it is a drag, not a click */
#define TURN 0.005f       /* radians a point of drag turns the camera, at camera_speed 1 */
#define PITCH_MIN -0.8f   /* the eye's angle over the look-at point: under it (looking up; the game keeps it out of the ground) */
#define PITCH_MAX 1.35f   /* over it, nearly straight down */
#define ZOOM_STEP 0.87f   /* a wheel notch in */
#define GAME_NEAR 3.0f    /* the game eases its eye out from within this, not straight back: no zoom in past it */
#define GAME_FAR 6.0f     /* and in from beyond this, straight back */
#define ZOOM_MAX 24.0f
#define DIK_NUMPAD8 0x48

static int g_on_given, g_on;
static float g_speed = 1.0f;

/* the mouse, on the window's thread (the game's) */
static int g_held;            /* SDL_BUTTON_MASK bits of the presses kept */
static int g_dragging, g_running, g_captured;
static SDL_Event g_press;     /* the first press kept, while it may still be a click */
static float g_press_x, g_press_y;
static float g_turn_x, g_turn_y, g_wheel; /* since the last frame */

static float g_dist; /* the zoom: the eye's distance, 0 as the game keeps it */
static int g_aiming;  /* a drag's aim, held from the frame it began */
static float g_yaw, g_pitch, g_len;

static uint32_t camera_object(void)
{
#if FFXI_MODERN
    uint32_t scene = xi_game_rd32(FFXI_MODERN_DISP_SCENE);
    uint32_t c = scene ? xi_game_rd32(scene + 0x50) : 0;
    return c && xi_mapped(c, 0x60) ? c : 0;
#else
    return 0;
#endif
}

static int rd_vec(uint32_t a, float v[3]) { return xi_game_rd(a, v, 12); }

static void wr_vec(uint32_t a, const float v[3])
{
    if (xi_mapped(a, 12))
        memcpy(GUEST_PTR(a), v, 12);
}

/* The player's position (x, height, z, as the camera has them) and status, from its entity. */
static int player(float pos[3], uint32_t* status)
{
    static uint32_t movement = 0xFFFFFFFFu, at_status;
    if (movement == 0xFFFFFFFFu)
    {
        int sid = xi_game_struct_id("entity_t");
        uint32_t extra = 0;
        const xi_field* f = sid >= 0 ? xi_game_lookup(sid, "Movement.LocalPosition.X", &extra) : NULL;
        const xi_field* s = sid >= 0 ? xi_game_lookup(sid, "Status", NULL) : NULL;
        movement = f ? extra + f->offset : 0;
        at_status = s ? s->offset : 0;
    }
    int32_t i = xi_game_player_index();
    uint32_t e = i >= 0 ? xi_game_entity((uint32_t)i) : 0;
    if (!e || !movement || !at_status)
        return 0;
    /* the entity's X, Z (up) and Y are the camera's x, height and z */
    if (!rd_vec(e + movement, pos))
        return 0;
    *status = xi_game_rd32(e + at_status);
    return 1;
}

/* The camera, when it is the game's own following the character: 1 third person, 2 first, 0 not
 * ours to touch (no camera, signing in, a cutscene, a camera off somewhere else). */
static int follow(uint32_t* cam, float eye[3], float at[3])
{
    float me[3];
    uint32_t status;
    *cam = camera_object();
    if (!*cam || xi_game_login_status() != 2 || !rd_vec(*cam + 0x44, eye) || !rd_vec(*cam + 0x50, at) ||
        !player(me, &status) || status == 4 /* an event */)
        return 0;
    for (int k = 0; k < 3; ++k)
        if (!isfinite(eye[k]) || !isfinite(at[k]))
            return 0;
    float ax = at[0] - me[0], az = at[2] - me[2], ex = eye[0] - me[0], ez = eye[2] - me[2];
    if (ax * ax + az * az < 4.0f)
        return 1; /* looking at the character */
    return ex * ex + ez * ez < 0.36f ? 2 : 0; /* the eye in its head */
}

/* third person, following the character: the mouse is the orbit camera's */
static int in_world(void)
{
    uint32_t cam;
    float eye[3], at[3];
    return follow(&cam, eye, at) == 1;
}

static int over_world(float x, float y, int w, int h)
{
    return w > 0 && h > 0 && in_world() && !(user32_ui_hit && user32_ui_hit((x + 0.5f) / (float)w, (y + 0.5f) / (float)h));
}

static void run(int on)
{
    if (on != g_running)
        input_inject_key(DIK_NUMPAD8, on), g_running = on;
}

static void drag_start(SDL_MouseID which)
{
    g_dragging = 1;
    g_captured = which != USER32_INJECTED_MOUSE; /* the control port's has no cursor to hold */
    if (g_captured)
        user32_mouse_capture(1);
}

static void drag_end(void)
{
    run(0);
    if (g_captured)
        user32_mouse_capture(0);
    g_dragging = g_captured = g_held = 0;
}

static int mouse(const void* event, int w, int h)
{
    const SDL_Event* e = (const SDL_Event*)event;
    if (!g_on)
    {
        if (g_held && e->type == SDL_EVENT_MOUSE_BUTTON_UP)
            drag_end();
        return 0;
    }
    switch (e->type)
    {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    {
        int b = e->button.button;
        if (b != SDL_BUTTON_LEFT && b != SDL_BUTTON_RIGHT)
            return 0;
        if (g_held)
        {
            /* the other button too: a drag that runs */
            g_held |= SDL_BUTTON_MASK(b);
            if (!g_dragging)
                drag_start(e->button.which);
            run(1);
            return 1;
        }
        if (!in_world())
            return 0;
        g_held = SDL_BUTTON_MASK(b);
        g_press = *e, g_press_x = e->button.x, g_press_y = e->button.y;
        return 1;
    }
    case SDL_EVENT_MOUSE_MOTION:
        if (!g_held)
            return 0;
        if (!g_dragging)
        {
            float dx = e->motion.x - g_press_x, dy = e->motion.y - g_press_y;
            if (dx * dx + dy * dy < DRAG_POINTS * DRAG_POINTS)
                return 1;
            drag_start(e->motion.which);
        }
        g_turn_x += e->motion.xrel, g_turn_y += e->motion.yrel;
        return 1;
    case SDL_EVENT_MOUSE_BUTTON_UP:
    {
        int m = SDL_BUTTON_MASK(e->button.button);
        if (!(g_held & m))
            return 0;
        g_held &= ~m;
        if (!g_dragging)
        {
            /* a click: the game gets the press, then this release */
            user32_replay(&g_press);
            return 0;
        }
        run(0);
        if (!g_held)
            drag_end();
        return 1;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (g_held || !over_world(e->wheel.mouse_x, e->wheel.mouse_y, w, h))
            return 0;
        g_wheel += e->wheel.y;
        return 1;
    default: return 0;
    }
}

/* v's angle round the up axis and over the level, and its length */
static void angles(const float v[3], float* yaw, float* pitch, float* len)
{
    float r = sqrtf(v[0] * v[0] + v[2] * v[2]);
    *yaw = atan2f(v[2], v[0]);
    *pitch = atan2f(-v[1], r); /* the height grows downward */
    *len = sqrtf(r * r + v[1] * v[1]);
}

static void from_angles(float yaw, float pitch, float len, float v[3])
{
    v[0] = len * cosf(pitch) * cosf(yaw);
    v[1] = -len * sinf(pitch);
    v[2] = len * cosf(pitch) * sinf(yaw);
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

void camera_frame(void)
{
    float live = gfx_fx_get("camera");
    int on = live >= 0.5f && live < 1.5f ? 1 : live >= 1.5f ? 0 : g_on_given;
    float speed = gfx_fx_get("camera_speed");
    g_speed = speed > 0.0f ? speed : 1.0f;
    if (on != g_on)
    {
        g_on = on;
        if (!on)
        {
            if (g_held)
                drag_end();
            g_dist = 0.0f;
        }
    }
    float tx = g_turn_x * TURN * g_speed, ty = g_turn_y * TURN * g_speed, wheel = g_wheel;
    g_turn_x = g_turn_y = g_wheel = 0.0f;
    uint32_t cam;
    float eye[3], at[3];
    int mode = g_on ? follow(&cam, eye, at) : 0;
    if (!mode)
    {
        if (g_dragging)
            drag_end();
        return;
    }
    float yaw, pitch, len, v[3];
    if (mode == 2)
    {
        /* first person: the game sets the look-at point from its own aim every frame, so the mouse
         * is the game's there (in_world) */
        if (g_dragging)
            drag_end();
        return;
    }
    float o[3] = { eye[0] - at[0], eye[1] - at[1], eye[2] - at[2] };
    angles(o, &yaw, &pitch, &len);
    if (len < 0.01f)
        return;
    /* a drag holds its own aim from where the game had it, so the game's pushing the eye up out of
     * the ground (or its trailing a running character) does not creep into it */
    if (g_dragging)
    {
        if (!g_aiming)
            g_aiming = 1, g_yaw = yaw, g_pitch = pitch, g_len = len;
        g_yaw -= tx;
        g_pitch = clampf(g_pitch + ty, fminf(PITCH_MIN, g_pitch), fmaxf(PITCH_MAX, g_pitch));
        yaw = g_yaw, pitch = g_pitch, len = g_len;
    }
    else
        g_aiming = 0;
    if (wheel != 0.0f)
    {
        float d = clampf((g_dist > 0.0f ? g_dist : len) * powf(ZOOM_STEP, wheel), GAME_NEAR, ZOOM_MAX);
        /* back within the game's own range: the game keeps it from here */
        g_dist = d > GAME_FAR ? d : 0.0f;
        len = g_len = d;
    }
    if (!g_aiming && wheel == 0.0f && !g_dist)
        return;
    from_angles(yaw, pitch, g_dist > 0.0f ? g_dist : len, v);
    float to[3] = { at[0] + v[0], at[1] + v[1], at[2] + v[2] };
    wr_vec(cam + 0x44, to);
}

void camera_init(int on)
{
    g_on_given = g_on = on;
    user32_mouse_hook = mouse;
#if FFXI_MODERN
    rt_log("[recomp] camera: %s\n", on ? "orbit (the mouse turns it round the character)" : "the game's");
#else
    rt_log("[recomp] camera: build %s has no scene address; the game's camera\n", FFXI_BUILD);
#endif
}
