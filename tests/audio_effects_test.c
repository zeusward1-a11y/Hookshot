/* Synthetic signals through the real nn_audio.c delay and mix biquad filter (the section between "---- delay" and
   "fx_seq", with the defines and the Mix type it uses). No device, no game. */
#define _CRT_SECURE_NO_WARNINGS   /* (the section reads HOS_AUDIO_NO_DELAY with getenv) */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_effects.inc"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", scenario, __LINE__, #x); exit(1); } } while (0)

static const char *scenario;
static Mix mix;

static Delay make_delay(int channels, double ms, float in_gain, float fb, float dry, float lp, float spread) {
    Delay d;
    memset(&d, 0, sizeof d);
    d.used = 1, d.enabled = 1, d.n = channels;
    for (int c = 0; c < MAX_CH; c++) d.in[c] = d.out[c] = (int8_t)c;
    d.len = RATE;   /* 1 s */
    d.line = (float *)calloc((size_t)channels * (size_t)d.len, sizeof(float));
    d.time_ns = (int64_t)(ms * 1e6);
    d.in_gain = in_gain, d.fb_gain = fb, d.dry = dry, d.lp_amount = lp, d.spread = spread;
    return d;
}

/* an impulse at sample 0 comes back at the delay time, then at twice it scaled by the feedback */
static void delay_echoes(void) {
    scenario = "delay echoes";
    memset(&mix, 0, sizeof mix);
    mix.buffers = 1;
    Delay d = make_delay(1, 10.0, 1.0f, 0.5f, 0.0f, 0.0f, 0.0f);
    const int at = RATE / 100;   /* 10 ms */
    float out[RATE / 10];        /* 100 ms */
    int n = 0;
    for (int f = 0; n + FRAME <= (int)(sizeof out / sizeof out[0]); f++) {
        memset(mix.buf, 0, sizeof mix.buf);
        if (f == 0) mix.buf[0][0] = 1.0f;
        delay_frame(&d, &mix);
        memcpy(out + n, mix.buf[0], FRAME * sizeof(float));
        n += FRAME;
    }
    CHECK(fabsf(out[0]) < 1e-6f);                /* (no dry signal) */
    CHECK(fabsf(out[at] - 1.0f) < 1e-6f);        /* the first echo, whole */
    CHECK(fabsf(out[2 * at] - 0.5f) < 1e-6f);    /* the second, at the feedback */
    CHECK(fabsf(out[3 * at] - 0.25f) < 1e-6f);
    for (int i = 1; i < 3 * at; i++)
        if (i != at && i != 2 * at) CHECK(fabsf(out[i]) < 1e-6f);
    free(d.line);
}

/* dry gain passes the input through at once, beside the delayed copy */
static void delay_dry(void) {
    scenario = "delay dry";
    memset(&mix, 0, sizeof mix);
    mix.buffers = 1;
    Delay d = make_delay(1, 5.0, 1.0f, 0.0f, 0.8f, 0.0f, 0.0f);
    mix.buf[0][3] = 1.0f;
    delay_frame(&d, &mix);
    CHECK(fabsf(mix.buf[0][3] - 0.8f) < 1e-6f);
    free(d.line);
}

/* channel spread feeds one channel's echo back into the other */
static void delay_spread(void) {
    scenario = "delay spread";
    memset(&mix, 0, sizeof mix);
    mix.buffers = 2;
    Delay d = make_delay(2, 2.0, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f);
    const int at = RATE / 500;   /* 2 ms */
    float right[FRAME * 2];
    for (int f = 0; f < 2; f++) {
        memset(mix.buf, 0, sizeof mix.buf);
        if (f == 0) mix.buf[0][0] = 1.0f;   /* the left channel only */
        delay_frame(&d, &mix);
        memcpy(right + f * FRAME, mix.buf[1], FRAME * sizeof(float));
    }
    CHECK(fabsf(right[at]) < 1e-6f);             /* the first echo stays left */
    CHECK(fabsf(right[2 * at] - 1.0f) < 1e-6f);  /* its feedback went right */
    free(d.line);
}

/* a NaN input does not stay in the line */
static void delay_nan(void) {
    scenario = "delay nan";
    memset(&mix, 0, sizeof mix);
    mix.buffers = 1;
    Delay d = make_delay(1, 1.0, 1.0f, 0.9f, 1.0f, 0.3f, 0.0f);
    for (int f = 0; f < 8; f++) {
        memset(mix.buf, 0, sizeof mix.buf);
        if (f == 0) mix.buf[0][0] = NAN;
        delay_frame(&d, &mix);
        for (int i = 0; i < FRAME; i++) CHECK(isfinite(mix.buf[0][i]));
    }
    free(d.line);
}

static double rms_through(BqFx *f, double cycles_per_sample) {
    double sq = 0;
    int n = 0;
    for (int fr = 0; fr < 40; fr++) {
        memset(mix.buf, 0, sizeof mix.buf);
        for (int i = 0; i < FRAME; i++) mix.buf[0][i] = (float)sin(2 * 3.141592653589793 * cycles_per_sample * (fr * FRAME + i));
        bqfx_frame(f, &mix);
        if (fr >= 20)   /* (settled) */
            for (int i = 0; i < FRAME; i++) sq += (double)mix.buf[0][i] * mix.buf[0][i], n++;
    }
    return sqrt(sq / n);
}

/* a one-pole low pass in the additive form the game gives (b0 = 1 - p, a1 = p) keeps the lows, cuts the highs */
static void biquad_low_pass(void) {
    scenario = "biquad low pass";
    memset(&mix, 0, sizeof mix);
    mix.buffers = 1;
    BqFx f;
    memset(&f, 0, sizeof f);
    f.used = 1, f.enabled = 1, f.n = 1;
    const float p = 0.9f;
    f.b[0] = 1.0f - p, f.a[0] = p;
    const double low = rms_through(&f, 50.0 / RATE), high = rms_through(&f, 12000.0 / RATE);
    CHECK(low > 0.6);    /* 50 Hz: ~0.707 (a sine's rms) */
    CHECK(high < 0.06);  /* 12 kHz: down by more than 20 dB */
}

/* coefficients that blow up give silence and a fresh state, not a full-scale burst */
static void biquad_unstable(void) {
    scenario = "biquad unstable";
    memset(&mix, 0, sizeof mix);
    mix.buffers = 1;
    BqFx f;
    memset(&f, 0, sizeof f);
    f.used = 1, f.enabled = 1, f.n = 1;
    f.b[0] = 1.0f, f.a[0] = 3.0f;
    for (int fr = 0; fr < 20; fr++) {
        for (int i = 0; i < FRAME; i++) mix.buf[0][i] = 1.0f;
        bqfx_frame(&f, &mix);
        for (int i = 0; i < FRAME; i++) CHECK(isfinite(mix.buf[0][i]));
    }
    CHECK(isfinite(f.z[0][0]) && isfinite(f.z[0][1]));
}

int main(void) {
    delay_echoes();
    delay_dry();
    delay_spread();
    delay_nan();
    biquad_low_pass();
    biquad_unstable();
    puts("audio effects: 6 cases passed");
    return 0;
}
