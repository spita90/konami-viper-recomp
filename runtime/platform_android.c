/*
 * Android services for the runtime (see platform_mobile.h): the game's files out of the APK's
 * assets, the internal storage for the saves, the display cutout, tilt steering from the
 * accelerometer and a haptic tap for the touch buttons. The two that need the Activity (the cutout,
 * the tap) are methods of android/ViperActivity.java, called through JNI.
 */
#include "platform_mobile.h"
#include <SDL.h>
#include <SDL_system.h>
#include <jni.h>
#include <android/log.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void rt_log(const char *fmt, ...);

/* ------------------------------------------------------------------ the log */
/* stderr (rt_log and the rest) to logcat, tag "viper": Android throws an app's stderr away. A pipe
 * read by a thread of its own, line by line, set up as the library loads */
static int s_log_fd[2];

static void *log_thread(void *ud) {
    (void)ud;
    static char line[1024];
    size_t n = 0;
    char c;
    while (read(s_log_fd[0], &c, 1) == 1) {
        if (c != '\n' && n < sizeof line - 1) { line[n++] = c; continue; }
        line[n] = 0;
        __android_log_write(ANDROID_LOG_INFO, "viper", line);
        n = 0;
    }
    return NULL;
}

__attribute__((constructor)) static void stderr_to_logcat(void) {
    if (pipe(s_log_fd) != 0) return;
    dup2(s_log_fd[1], STDERR_FILENO);
    setvbuf(stderr, NULL, _IOLBF, 0);
    pthread_t t;
    if (pthread_create(&t, NULL, log_thread, NULL) == 0) pthread_detach(t);
}

const char *mobile_data_dir(void) {
    const char *p = SDL_AndroidGetInternalStoragePath();
    return p ? p : ".";
}

/* ------------------------------------------------------------------ the game's files */
/* The assets hold the files at their paths beside the executable, and viper_files.txt: a first
 * line naming this build (a stamp), then one path a line. They are copied to <internal>/game at
 * the first launch, and again after an update (another stamp), the stamp written last so that a
 * copy cut short is done again. */
static int mkdirs(char *path) {             /* the directories of path (its last part is the file) */
    for (char *p = path + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            int bad = mkdir(path, 0700) != 0 && errno != EEXIST;
            *p = '/';
            if (bad) return -1;
        }
    return 0;
}

static int copy_asset(const char *rel, const char *dst) {
    SDL_RWops *in = SDL_RWFromFile(rel, "rb");
    if (!in) { rt_log("android: asset %s: %s\n", rel, SDL_GetError()); return -1; }
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s.part", dst);
    FILE *out = fopen(tmp, "wb");
    if (!out) { SDL_RWclose(in); return -1; }
    static char buf[1 << 16];
    size_t n;
    int ok = 1;
    while ((n = SDL_RWread(in, buf, 1, sizeof buf)) > 0) ok &= fwrite(buf, 1, n, out) == n;
    SDL_RWclose(in);
    ok &= fclose(out) == 0;
    if (!ok || rename(tmp, dst) != 0) { remove(tmp); return -1; }
    return 0;
}

const char *mobile_game_dir(void) {
    static char dir[1024];
    if (dir[0]) return dir;
    snprintf(dir, sizeof dir, "%s/game", mobile_data_dir());
    mkdir(dir, 0700);
    size_t size = 0;
    char *list = SDL_LoadFile("viper_files.txt", &size);    /* an asset */
    if (!list) { rt_log("android: no viper_files.txt in the assets: %s\n", SDL_GetError()); return dir; }
    char *line = strtok(list, "\n");
    char stamp_path[1100], have[128] = "";
    snprintf(stamp_path, sizeof stamp_path, "%s/.stamp", dir);
    FILE *f = fopen(stamp_path, "r");
    if (f) { if (!fgets(have, sizeof have, f)) have[0] = 0; fclose(f); have[strcspn(have, "\n")] = 0; }
    if (line && strcmp(have, line) != 0) {
        char stamp[128];
        snprintf(stamp, sizeof stamp, "%s", line);
        rt_log("android: copying the game's files out of the APK (%s)\n", stamp);
        remove(stamp_path);
        int ok = 1;
        while ((line = strtok(NULL, "\n")) != NULL) {
            if (!*line) continue;
            char dst[1100];
            snprintf(dst, sizeof dst, "%s/%s", dir, line);
            ok &= mkdirs(dst) == 0 && copy_asset(line, dst) == 0;
        }
        if (ok && (f = fopen(stamp_path, "w")) != NULL) { fprintf(f, "%s\n", stamp); fclose(f); }
        else rt_log("android: the game's files could not all be copied\n");
    }
    SDL_free(list);
    return dir;
}

/* ------------------------------------------------------------------ the Activity (JNI) */
static jobject activity_call(const char *name, const char *sig, JNIEnv **envp) {
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject act = (jobject)SDL_AndroidGetActivity();
    *envp = env;
    if (!env || !act) return NULL;
    jclass cls = (*env)->GetObjectClass(env, act);
    jmethodID m = (*env)->GetMethodID(env, cls, name, sig);
    (*env)->DeleteLocalRef(env, cls);
    if (!m) { (*env)->ExceptionClear(env); (*env)->DeleteLocalRef(env, act); return NULL; }
    jobject r = NULL;
    if (sig[strlen(sig) - 1] == 'V') (*env)->CallVoidMethod(env, act, m);
    else r = (*env)->CallObjectMethod(env, act, m);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); r = NULL; }
    (*env)->DeleteLocalRef(env, act);
    return r;
}

/* the display cutout's insets (pixels, the window's units on Android), asked once */
void mobile_safe_area(float *left, float *top, float *right, float *bottom) {
    static int done, in[4];
    if (!done) {
        done = 1;
        JNIEnv *env;
        jintArray a = (jintArray)activity_call("safeInsets", "()[I", &env);
        if (a && (*env)->GetArrayLength(env, a) == 4) (*env)->GetIntArrayRegion(env, a, 0, 4, in);
        if (a) (*env)->DeleteLocalRef(env, a);
    }
    *left = (float)in[0];
    *top = (float)in[1];
    *right = (float)in[2];
    *bottom = (float)in[3];
}

void mobile_haptic_tap(void) {
    JNIEnv *env;
    activity_call("hapticTap", "()V", &env);
}

/* ------------------------------------------------------------------ tilt */
static SDL_Sensor *s_accel;
static int s_accel_checked, s_accel_index = -1;

int mobile_tilt_available(void) {
    if (!s_accel_checked) {
        s_accel_checked = 1;
        if (SDL_InitSubSystem(SDL_INIT_SENSOR) == 0)
            for (int i = 0; i < SDL_NumSensors(); i++)
                if (SDL_SensorGetDeviceType(i) == SDL_SENSOR_ACCEL) { s_accel_index = i; break; }
    }
    return s_accel_index >= 0;
}

void mobile_tilt_enable(int on) {
    if (!mobile_tilt_available()) return;
    if (on && !s_accel) s_accel = SDL_SensorOpen(s_accel_index);
    else if (!on && s_accel) { SDL_SensorClose(s_accel); s_accel = NULL; }
}

/* The accelerometer gives the reaction to gravity in the device's portrait frame (x right, y up),
 * m/s^2: gravity is its opposite. Landscape with the right side up: the screen's down is -x and its
 * right -y; flipped, +x and +y (the same as iOS's, platform_ios.m). */
int mobile_tilt(double *angle) {
    *angle = 0;
    float a[3];
    if (!s_accel || SDL_SensorGetData(s_accel, a, 3) != 0) return 0;
    double gx = -a[0] / SDL_STANDARD_GRAVITY, gy = -a[1] / SDL_STANDARD_GRAVITY;
    double down, right;
    switch (SDL_GetDisplayOrientation(0)) {
    case SDL_ORIENTATION_LANDSCAPE: down = -gx; right = -gy; break;
    case SDL_ORIENTATION_LANDSCAPE_FLIPPED: down = gx; right = gy; break;
    case SDL_ORIENTATION_PORTRAIT_FLIPPED: down = gy; right = -gx; break;
    default: down = -gy; right = gx; break;
    }
    if (fabs(down) + fabs(right) < 0.2) return 0;   /* lying flat: no steering from it */
    *angle = atan2(right, down);
    return 1;
}
