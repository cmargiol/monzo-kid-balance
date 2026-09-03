/**
 * Snake, steered by tilting the device. Second screen in the cycle: press B
 * from the balance screen.
 *
 * Controls (tilt steering, the default):
 *   tilt   steer — a lean ACROSS the direction of travel turns the snake
 *          that way (heading right: lean down to turn down; heading up: lean
 *          right to turn right); lean along the heading does nothing, so
 *          after a turn the same lean doesn't keep turning it. "Level" is
 *          however the device was held when the round started, so it works
 *          at any resting angle
 *   A      start / pause / play again
 *   B      leave to the next demo screen
 *
 * Fallback if tilt annoys the player — set SNAKE_BUTTON_STEERING to 1:
 *   A      turn left        B  turn right
 *   (leave via B from the start or game-over screen)
 *
 * The start screen shows the live tilt reading, which is how the axis
 * mapping below was verified on the device.
 */
#include "test.h"

#define SNAKE_BUTTON_STEERING 0

namespace TEST
{

    static const int CELL = 8;
    static const int TOP = 15;                 // score bar height
    static const int COLS = 240 / CELL;        // 30
    static const int ROWS = (135 - TOP) / CELL; // 15
    static const int MAX_LEN = COLS * ROWS;

    static const uint32_t START_STEP_MS = 280; // slow enough for a 7-year-old
    static const uint32_t MIN_STEP_MS = 130;
    static const uint32_t SPEEDUP_MS = 8;      // per food eaten

    // Early food stays clear of the edges; the clear margin shrinks by one
    // cell every FOOD_MARGIN_STEP foods until anything goes.
    static const int FOOD_MARGIN_START = 4;
    static const int FOOD_MARGIN_STEP = 3;

    // Tilt as angles relative to the calibrated neutral, so the dead zone
    // means the same lean whether the device rests flat or stands up. 15° is
    // deliberate but keeps the screen readable.
    static const float TILT_DEAD_DEG = 15.0f;
    // The IMU is mounted rotated relative to the screen: its y axis runs
    // along the screen's long side (left-right) and its x axis along the
    // short side (up-down). Signs were read off the start-screen readout and
    // set so that left edge down is negative LR and tipping the top edge
    // away from you is negative UD — "push forward" steers up, like a
    // joystick, rather than like a marble rolling to the low edge.
    static const float LR_SIGN = -1.0f;
    static const float UD_SIGN = -1.0f;
    static const uint32_t PAUSE_KEEPAWAKE_MS = 10 * 60 * 1000;

    enum Dir { UP, RIGHT, DOWN, LEFT, NONE };
    static const int DX[4] = {0, 1, 0, -1};
    static const int DY[4] = {-1, 0, 1, 0};
    static const char *DIR_NAME[5] = {"UP", "RIGHT", "DOWN", "LEFT", "-"};

    struct Snake
    {
        uint8_t x[MAX_LEN], y[MAX_LEN]; // ring buffer of cells
        int head, len;                  // head index and length
        Dir dir, next;
        int foodX, foodY;
        int score;
        uint32_t stepMs;
    };
    static Snake S;
    static float _neutralRoll = 0, _neutralPitch = 0;
    static Preferences _snakePrefs; // high score, survives power-off
    static int _best = 0;

    /** Lean angles in degrees, screen-relative: `roll` is left/right (bank
     *  formula on chip y, defined at any hold), `pitch` is up/down (atan2
     *  of chip x against z — stable unless the device stands on a short
     *  edge, which is not a way anyone plays). */
    static void tiltAngles(MPU6886 &imu, float &roll, float &pitch)
    {
        float ax, ay, az;
        imu.getAccelData(&ax, &ay, &az);
        roll = atan2f(ay, sqrtf(ax * ax + az * az)) * 57.2958f;
        pitch = atan2f(ax, az) * 57.2958f;
    }

    static float wrapDeg(float d)
    {
        while (d > 180) d -= 360;
        while (d < -180) d += 360;
        return d;
    }

    static void calibrate(MPU6886 &imu)
    {
        float r, p, sr = 0, sp = 0;
        for (int i = 0; i < 10; i++)
        {
            tiltAngles(imu, r, p);
            sr += r;
            sp += p;
            delay(10);
        }
        _neutralRoll = sr / 10;
        _neutralPitch = sp / 10;
    }

    /** Lean relative to neutral: lr > 0 is right, ud > 0 is down. */
    static void tiltLean(MPU6886 &imu, float &lr, float &ud)
    {
        float roll, pitch;
        tiltAngles(imu, roll, pitch);
        lr = LR_SIGN * wrapDeg(roll - _neutralRoll);
        ud = UD_SIGN * wrapDeg(pitch - _neutralPitch);
    }

    /** Absolute lean direction — the start screen's readout for checking
     *  the axis mapping. Not used to steer. */
    static Dir tiltDir(MPU6886 &imu)
    {
        float lr, ud;
        tiltLean(imu, lr, ud);
        if (fabsf(lr) < TILT_DEAD_DEG && fabsf(ud) < TILT_DEAD_DEG)
            return NONE;
        if (fabsf(lr) >= fabsf(ud))
            return lr > 0 ? RIGHT : LEFT;
        return ud > 0 ? DOWN : UP;
    }

    /** Steering: only the lean across the direction of travel counts. */
    static Dir tiltTurn(MPU6886 &imu, Dir heading)
    {
        float lr, ud;
        tiltLean(imu, lr, ud);
        bool vertical = (heading == UP || heading == DOWN);
        float across = vertical ? lr : ud;
        if (fabsf(across) < TILT_DEAD_DEG)
            return NONE;
        if (vertical)
            return across > 0 ? RIGHT : LEFT;
        return across > 0 ? DOWN : UP;
    }

    /** Is (x, y) one of the first `count` segments from the head? */
    static bool onSnake(int x, int y, int count)
    {
        for (int i = 0; i < count; i++)
        {
            int k = (S.head - i + MAX_LEN) % MAX_LEN;
            if (S.x[k] == x && S.y[k] == y)
                return true;
        }
        return false;
    }

    static void placeFood()
    {
        int margin = FOOD_MARGIN_START - S.score / FOOD_MARGIN_STEP;
        if (margin < 0)
            margin = 0;
        do
        {
            S.foodX = margin + random(COLS - 2 * margin);
            S.foodY = margin + random(ROWS - 2 * margin);
        } while (onSnake(S.foodX, S.foodY, S.len));
    }

    static void resetGame()
    {
        S.head = 0;
        S.len = 3;
        S.x[0] = COLS / 2;
        S.y[0] = ROWS / 2;
        // Two more segments trailing to the left, stored behind the head.
        S.x[MAX_LEN - 1] = S.x[0] - 1;
        S.y[MAX_LEN - 1] = S.y[0];
        S.x[MAX_LEN - 2] = S.x[0] - 2;
        S.y[MAX_LEN - 2] = S.y[0];
        S.dir = S.next = RIGHT;
        S.score = 0;
        S.stepMs = START_STEP_MS;
        placeFood();
    }

    /** Advance one cell. Returns false when the snake dies. */
    static bool step()
    {
        S.dir = S.next;
        int nx = S.x[S.head] + DX[S.dir];
        int ny = S.y[S.head] + DY[S.dir];
        if (nx < 0 || nx >= COLS || ny < 0 || ny >= ROWS)
            return false;
        bool eats = (nx == S.foodX && ny == S.foodY);
        // Unless eating, the tail moves away this step, so it can't collide.
        if (onSnake(nx, ny, eats ? S.len : S.len - 1))
            return false;
        S.head = (S.head + 1) % MAX_LEN;
        S.x[S.head] = nx;
        S.y[S.head] = ny;
        if (eats)
        {
            S.len++;
            S.score++;
            if (S.stepMs > MIN_STEP_MS + SPEEDUP_MS)
                S.stepMs -= SPEEDUP_MS;
            placeFood();
        }
        return true;
    }

    void TEST::snake_draw_board()
    {
        Disbuff->fillRect(0, 0, 240, 135, TFT_BLACK);
        Disbuff->fillRect(0, 0, 240, TOP, Disbuff->color565(20, 20, 20));
        Disbuff->setFont(&fonts::Font0);
        Disbuff->setTextSize(1);
        Disbuff->setTextColor(TFT_WHITE);
        Disbuff->setCursor(4, 4);
        Disbuff->printf("SNAKE   %d", S.score);

        Disbuff->fillRoundRect(S.foodX * CELL + 1, TOP + S.foodY * CELL + 1, CELL - 2, CELL - 2, 3, TFT_RED);
        for (int i = 0; i < S.len; i++)
        {
            int k = (S.head - i + MAX_LEN) % MAX_LEN;
            uint16_t c = (i == 0) ? Disbuff->color565(190, 255, 190) : TFT_GREEN;
            Disbuff->fillRect(S.x[k] * CELL + 1, TOP + S.y[k] * CELL + 1, CELL - 2, CELL - 2, c);
        }
        Displaybuff();
    }

    void TEST::snake_draw_message(const char *title, const char *line2, const char *footer)
    {
        Disbuff->fillRect(0, 0, 240, 135, TFT_BLACK);
        Disbuff->setFont(&fonts::Font0);
        Disbuff->setTextSize(3);
        Disbuff->setTextColor(TFT_GREEN);
        Disbuff->drawCenterString(title, 120, 22);
        Disbuff->setTextSize(2);
        Disbuff->setTextColor(TFT_WHITE);
        Disbuff->drawCenterString(line2, 120, 66);
        Disbuff->setTextSize(1);
        Disbuff->setTextColor(Disbuff->color565(150, 150, 150));
        Disbuff->drawCenterString(footer, 120, 118);
        Displaybuff();
    }

    void TEST::snake_app()
    {
        enum { START, PLAYING, PAUSED, OVER } state = START;
        char line[40];
        uint32_t lastStep = 0, lastIndicator = 0, pausedAt = 0;
        char footer[40];
        static bool prefsOpen = false;
        if (!prefsOpen)
        {
            _snakePrefs.begin("snake");
            _best = _snakePrefs.getInt("best", 0);
            prefsOpen = true;
        }
        calibrate(imu); // so the start screen's live readout means something

        while (1)
        {
            checkReboot();
            // A press on a dimmed screen only wakes it (the device-wide rule);
            // mid-round the screen is never dimmed because play is activity.
            bool dimmed = power_dimmed();

            if (state == START)
            {
                if (millis() - lastIndicator > 100)
                {
                    lastIndicator = millis();
#if SNAKE_BUTTON_STEERING
                    snprintf(line, sizeof line, "A: left   B: right");
#else
                    float lr, ud;
                    tiltLean(imu, lr, ud);
                    snprintf(line, sizeof line, "LR %+3.0f UD %+3.0f %s", lr, ud, DIR_NAME[tiltDir(imu)]);
#endif
                    snprintf(footer, sizeof footer, "best %d      A: play      B: leave", _best);
                    snake_draw_message("SNAKE", line, footer);
                }
            }

            if (btnA.pressed())
            {
                power_input();
                if (dimmed)
                {
                }
                else if (state == START || state == OVER)
                {
                    // Let the hands settle before "level" is measured.
                    for (int n = 3; n > 0; n--)
                    {
                        snprintf(line, sizeof line, "%d", n);
                        snake_draw_message("HOLD STILL", line, "level is measured now");
                        _tone(1500, 40);
                        delay(600);
                    }
                    _tone(2500, 60);
                    calibrate(imu);
                    resetGame();
                    snake_draw_board();
                    lastStep = millis();
                    state = PLAYING;
                }
#if SNAKE_BUTTON_STEERING
                else if (state == PLAYING)
                {
                    S.next = (Dir)((S.dir + 3) % 4); // turn left
                }
#else
                else if (state == PLAYING)
                {
                    state = PAUSED;
                    pausedAt = millis();
                    snake_draw_message("PAUSED", "", "A: go on     B: leave");
                }
                else if (state == PAUSED)
                {
                    calibrate(imu); // hands may have moved
                    lastStep = millis();
                    state = PLAYING;
                }
#endif
            }

            if (btnB.pressed())
            {
                power_input();
                if (dimmed)
                {
                }
#if SNAKE_BUTTON_STEERING
                else if (state == PLAYING)
                {
                    S.next = (Dir)((S.dir + 1) % 4); // turn right
                }
#endif
                else
                {
                    _tone(5500, 50);
                    return; // next screen in the cycle
                }
            }

            if (state == PAUSED && millis() - pausedAt < PAUSE_KEEPAWAKE_MS)
            {
                power_input(); // a paused round stays up for ten minutes
            }

            if (state == PLAYING)
            {
                power_input(); // steering by tilt counts as activity
#if !SNAKE_BUTTON_STEERING
                Dir d = tiltTurn(imu, S.dir);
                if (d != NONE)
                    S.next = d;
#endif
                if (millis() - lastStep >= S.stepMs)
                {
                    lastStep = millis();
                    int before = S.score;
                    if (step())
                    {
                        if (S.score > before)
                            _tone(2000, 30);
                        snake_draw_board();
                    }
                    else
                    {
                        if (S.score > _best)
                        {
                            _best = S.score;
                            _snakePrefs.putInt("best", _best);
                            snprintf(line, sizeof line, "NEW BEST %d!", S.score);
                            snake_draw_message("GAME OVER", line, "A: again     B: leave");
                            _tone(1500, 80); delay(90);
                            _tone(2000, 80); delay(90);
                            _tone(2600, 160);
                        }
                        else
                        {
                            _tone(300, 300);
                            snprintf(line, sizeof line, "score %d   best %d", S.score, _best);
                            snake_draw_message("GAME OVER", line, "A: again     B: leave");
                        }
                        state = OVER;
                    }
                }
            }

            delay(10);
        }
    }

}
