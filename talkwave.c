/*
 * talkwave — "listening" indicator + WAV recorder.
 *
 * Captures 16-bit signed LE, mono, 16000 Hz from a PulseAudio/PipeWire source
 * via libpulse (pa_simple) and shows a small bottom-centre bar whose level
 * meter tracks your voice. Writes a finalized WAV when told to stop.
 *
 * THREADING (this is the whole point): capture runs on its own thread and the
 * UI never blocks it. Earlier single-loop versions interleaved
 * pa_simple_read() with rendering, so frame pacing throttled how fast audio
 * was drained -- the backlog queued server-side and was discarded at SIGTERM,
 * silently losing ~45-70% of the recording. Audio must never wait on pixels.
 *
 * Options:
 *   --out <file>    where to write the WAV (default /tmp/talk.wav)
 *   --source <name> source to capture (default: the default source / mic)
 *
 * Signals:
 *   SIGUSR1         stop capturing, write the WAV, and keep the bar up showing
 *                   a "processing" sweep until SIGTERM (or PROC_MAX_MS passes).
 *                   This is what `talk stop` sends, so the bar covers the wait
 *                   for the transcription.
 *   SIGTERM/SIGINT  stop capturing if still going, write the WAV, exit.
 *   Window close is treated like SIGTERM.
 *
 * The WAV is written to <out>.part and renamed into place, so a non-empty
 * <out> always means a complete file.
 */
#include <SDL2/SDL.h>
#include <pulse/simple.h>
#include <pulse/pulseaudio.h>

#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SR         16000
#define NBARS      16
#define BAR_W      280
#define BAR_H      48
#define BAR_MARG   24
#define READ_BYTES 1280        /* 40 ms of s16le mono */
#define FRAME_MS   33          /* ~30 fps UI */
#define SWEEP_BARS 3           /* width of the moving group while processing */
#define SWEEP_MS   66          /* one bar step of the sweep */
#define PROC_MAX_MS 60000      /* never linger longer than this after SIGUSR1 */

/* Palette, matched to cleanTTS. */
#define C_BG      0x17, 0x1a, 0x21
#define C_BORDER  0x2a, 0x2f, 0x3a
#define C_DIM     0x2a, 0x2f, 0x3a
#define C_LIT     0x5e, 0xa1, 0xff

static volatile sig_atomic_t g_stop = 0;     /* exit */
static volatile sig_atomic_t g_finish = 0;   /* end capture, write WAV, show processing */
static void on_sig(int s) { (void)s; g_stop = 1; }
static void on_usr1(int s) { (void)s; g_finish = 1; }

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
static double g_t0;

/* Shared between the capture thread and the UI thread. */
typedef struct {
    pa_simple *pa;
    uint8_t   *pcm;
    size_t     len, cap;
    int        peak;           /* max |sample| since the UI last looked */
    SDL_mutex *lock;
} Cap;

static void write_wav_header(FILE *f, uint32_t data_size) {
    uint32_t chunk_size = 36 + data_size;
    uint16_t fmt = 1, ch = 1, bits = 16;
    uint32_t rate = SR;
    uint16_t block_align = ch * bits / 8;
    uint32_t byte_rate = rate * block_align;
    uint32_t fmtsize = 16;         /* written as 4 bytes below */
    fwrite("RIFF",1,4,f); fwrite(&chunk_size,4,1,f); fwrite("WAVE",1,4,f);
    fwrite("fmt ",1,4,f); fwrite(&fmtsize,4,1,f);
    fwrite(&fmt,2,1,f); fwrite(&ch,2,1,f); fwrite(&rate,4,1,f);
    fwrite(&byte_rate,4,1,f); fwrite(&block_align,2,1,f); fwrite(&bits,2,1,f);
    fwrite("data",1,4,f); fwrite(&data_size,4,1,f);
}

/* Capture thread: drain the stream as fast as it produces, nothing else. */
static int capture_thread(void *arg) {
    Cap *c = (Cap *)arg;
    uint8_t buf[READ_BYTES];
    while (!g_stop && !g_finish) {
        int err = 0;
        if (pa_simple_read(c->pa, buf, sizeof buf, &err) < 0) {
            if (!g_stop && !g_finish)
                fprintf(stderr, "pa_simple_read: %s\n", pa_strerror(err));
            /* Capture is dead: end the recording rather than leave the meter
             * up pretending to listen. What was captured so far is still written. */
            g_stop = 1;
            break;
        }
        int peak = 0;
        const int16_t *smp = (const int16_t *)buf;
        for (size_t i = 0; i < sizeof buf / 2; i++) {
            int a = abs(smp[i]);
            if (a > peak) peak = a;
        }
        SDL_LockMutex(c->lock);
        if (c->len + sizeof buf > c->cap) {
            size_t ncap = c->cap ? c->cap * 2 : 262144;
            while (ncap < c->len + sizeof buf) ncap *= 2;
            uint8_t *np = realloc(c->pcm, ncap);
            if (!np) { SDL_UnlockMutex(c->lock); fprintf(stderr, "OOM\n"); g_stop = 1; break; }
            c->pcm = np; c->cap = ncap;
        }
        memcpy(c->pcm + c->len, buf, sizeof buf);
        c->len += sizeof buf;
        if (peak > c->peak) c->peak = peak;
        SDL_UnlockMutex(c->lock);
    }
    return 0;
}

/* Write header + PCM to <path>.part, then rename over <path>. Mode 0600: this
 * is a microphone recording. Returns 0 on success. */
static int write_wav(const char *path, const uint8_t *pcm, size_t len) {
    char part[4096];
    if (snprintf(part, sizeof part, "%s.part", path) >= (int)sizeof part) {
        fprintf(stderr, "output path too long\n");
        return -1;
    }
    int fd = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    FILE *f = fd >= 0 ? fdopen(fd, "wb") : NULL;
    if (!f) {
        fprintf(stderr, "open %s: ", part); perror("");
        if (fd >= 0) close(fd);
        return -1;
    }
    write_wav_header(f, (uint32_t)len);
    if (len) fwrite(pcm, 1, len, f);
    if (fclose(f) != 0) { fprintf(stderr, "write %s: ", part); perror(""); return -1; }
    if (rename(part, path) != 0) { fprintf(stderr, "rename %s: ", path); perror(""); return -1; }
    return 0;
}

/* lit: bars lit from the left (level meter).
 * sweep: -1 for the meter; otherwise the processing animation, a group of
 * SWEEP_BARS lit bars starting at this index, with the dot dimmed. */
static void draw_frame(SDL_Renderer *ren, int lit, int sweep) {
    SDL_Rect all = { 0, 0, BAR_W, BAR_H };
    SDL_SetRenderDrawColor(ren, C_BG, 255);
    SDL_RenderFillRect(ren, &all);
    SDL_SetRenderDrawColor(ren, C_BORDER, 255);
    SDL_RenderDrawRect(ren, &all);

    int dot = 10;
    SDL_Rect d = { 16, (BAR_H - dot) / 2, dot, dot };
    if (sweep < 0) SDL_SetRenderDrawColor(ren, C_LIT, 255);
    else           SDL_SetRenderDrawColor(ren, C_DIM, 255);
    SDL_RenderFillRect(ren, &d);

    int barx0 = 44, barw = 9, barstep = 12, barh = 26;
    int bary = (BAR_H - barh) / 2;
    for (int b = 0; b < NBARS; b++) {
        int on = (sweep < 0) ? (b < lit) : (b >= sweep && b < sweep + SWEEP_BARS);
        if (on) SDL_SetRenderDrawColor(ren, C_LIT, 255);
        else    SDL_SetRenderDrawColor(ren, C_DIM, 255);
        SDL_Rect bar = { barx0 + b * barstep, bary, barw, barh };
        SDL_RenderFillRect(ren, &bar);
    }
}

int main(int argc, char **argv) {
    const char *out_path = "/tmp/talk.wav";
    const char *src = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc) out_path = argv[i + 1];
        else if (!strcmp(argv[i], "--source") && i + 1 < argc) src = argv[i + 1];
    }
    setenv("SDL_VIDEO_WAYLAND_APP_ID", "talkwave", 1);
    setenv("SDL_VIDEO_WAYLAND_WM_CLASS", "talkwave", 1);
    /* The names SDL documents for the Wayland app_id / window class. */
    setenv("SDL_VIDEO_WAYLAND_WMCLASS", "talkwave", 1);
    setenv("SDL_APP_ID", "talkwave", 1);

    g_t0 = now_ms();
    signal(SIGTERM, on_sig);
    signal(SIGINT,  on_sig);
    signal(SIGUSR1, on_usr1);

    /* Open capture FIRST: SDL init costs real milliseconds and this is
     * push-to-talk, so the user is already speaking. */
    pa_sample_spec spec = { .format = PA_SAMPLE_S16LE, .rate = SR, .channels = 1 };
    /* The server's default record fragsize here is 64000 bytes -- TWO SECONDS
     * at 16k mono. That delays the first delivery and throws away the current
     * partial fragment when we stop, which is what made a 5 s press yield
     * ~4 s of audio. Ask for 40 ms fragments instead. */
    pa_buffer_attr attr = {
        .maxlength = (uint32_t)-1,
        .tlength   = (uint32_t)-1,
        .prebuf    = (uint32_t)-1,
        .minreq    = (uint32_t)-1,
        .fragsize  = READ_BYTES,
    };
    int perr = 0;
    Cap cap = {0};
    cap.pa = pa_simple_new(NULL, "talkwave", PA_STREAM_RECORD, src,
                           "talkwave capture", &spec, NULL, &attr, &perr);
    if (!cap.pa) {
        fprintf(stderr, "pa_simple_new (%s): %s\n",
                src ? src : "default source", pa_strerror(perr));
        return 1;
    }
    cap.lock = SDL_CreateMutex();
    SDL_Thread *th = SDL_CreateThread(capture_thread, "capture", &cap);
    if (!th) { fprintf(stderr, "SDL_CreateThread failed\n"); return 1; }
    fprintf(stderr, "[t] capture running at %.0f ms\n", now_ms() - g_t0);

    int have_ui = 1;
    SDL_Window *win = NULL; SDL_Renderer *ren = NULL;
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s (continuing headless)\n", SDL_GetError());
        have_ui = 0;
    }
    if (have_ui) {
        SDL_DisplayMode dm;
        if (SDL_GetDesktopDisplayMode(0, &dm) != 0) { dm.w = 1920; dm.h = 1080; }
        win = SDL_CreateWindow("talkwave", (dm.w - BAR_W) / 2, dm.h - BAR_H - BAR_MARG,
                               BAR_W, BAR_H,
                               SDL_WINDOW_BORDERLESS | SDL_WINDOW_SKIP_TASKBAR);
        if (win) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
        if (!win || !ren) {
            fprintf(stderr, "window/renderer: %s (continuing headless)\n", SDL_GetError());
            have_ui = 0;
        }
    }
    if (have_ui) {
        draw_frame(ren, 0, -1);
        SDL_RenderPresent(ren);
        fprintf(stderr, "[t] window mapped at %.0f ms\n", now_ms() - g_t0);
    }

    /* UI loop. Never touches the audio path except to peek at the level. */
    float level = 0.0f;
    while (!g_stop && !g_finish) {
        SDL_Event e;
        while (SDL_PollEvent(&e))
            if (e.type == SDL_QUIT) g_stop = 1;
        if (g_stop || g_finish) break;

        SDL_LockMutex(cap.lock);
        int peak = cap.peak; cap.peak = 0;
        SDL_UnlockMutex(cap.lock);

        float lvl = peak / 32768.0f;
        level = (lvl > level) ? lvl : level * 0.75f;   /* decay trail */
        if (level > 1.0f) level = 1.0f;

        if (have_ui) {
            draw_frame(ren, (int)(level * NBARS + 0.5f), -1);
            SDL_RenderPresent(ren);
        }
        SDL_Delay(FRAME_MS);
    }

    /* Either flag ends the capture thread's loop; let it finish its chunk. */
    SDL_WaitThread(th, NULL);

    if (write_wav(out_path, cap.pcm, cap.len) == 0)
        fprintf(stderr, "[t] wrote %.2f s\n", cap.len / (double)(SR * 2));

    pa_simple_free(cap.pa);

    /* Processing phase: the WAV is done and `talk` is waiting on the server.
     * Keep the bar up with a sweep until told to exit. Runs headless too, so
     * the PID stays ours until `talk` sends SIGTERM. */
    if (g_finish && !g_stop) {
        double p0 = now_ms();
        int span = NBARS - SWEEP_BARS;
        while (!g_stop && now_ms() - p0 < PROC_MAX_MS) {
            SDL_Event e;
            while (have_ui && SDL_PollEvent(&e))
                if (e.type == SDL_QUIT) g_stop = 1;
            if (have_ui) {
                int step = (int)((now_ms() - p0) / SWEEP_MS) % (2 * span);
                draw_frame(ren, 0, step <= span ? step : 2 * span - step);
                SDL_RenderPresent(ren);
            }
            SDL_Delay(FRAME_MS);
        }
    }

    free(cap.pcm);
    SDL_DestroyMutex(cap.lock);
    if (ren) SDL_DestroyRenderer(ren);
    if (win) SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
