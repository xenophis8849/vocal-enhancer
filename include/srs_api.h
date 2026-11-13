/* vocal-enhancer C API (SRS vocal restoration, streaming, arm64).
 *
 *   int sup = srs_cpu_support();                 // 0 = unsupported CPU (hide the feature), 1 = dotprod, 2 = i8mm,
 *                                                // 3 = fp16 only (no int8 dot product: slower, outputs differ slightly)
 *   srs_engine* e = NULL;
 *   int rc = srs_create("srs_int8_v1.bin", 44100, 4, &e);
 *   while (recording) { srs_push(e, pcm, n); while ((k = srs_pull(e, buf, cap)) > 0) consume(buf, k); }
 *   srs_flush(e); while ((k = srs_pull(e, buf, cap)) > 0) consume(buf, k);   // total output == total input
 *   srs_destroy(e);
 *
 * Mono float PCM in [-1, 1] at 44100 or 48000 Hz; output at the same rate, same total length, peak <= -0.3 dBFS.
 * The model looks ~5.5 s ahead: output starts ~7 s after input starts (batching included); the tail arrives at srs_flush().
 * One engine per stream; an engine is not thread-safe (call it from one thread). Calls block while computing.
 * Threading: the network runs on batches of 32 frames (~1.4 s of audio), so most srs_push() calls return in ~1 ms and
 * one in ~15 does the whole batch (measured up to ~0.3 s on a Snapdragon 855 with 4 threads). Never call it from the
 * audio / recording callback: queue the PCM there and push from a worker thread.
 * threads = 0 (srs_create_* / srs_set_mode): automatic. SRS_MODE_LIVE starts with 1 thread and follows the measured
 * load (compute time / audio time over ~4 s): above 0.75 one more thread, below 0.35 one fewer, at most the file-mode
 * count; if that still cannot keep up it stays there and the rest is processed after recording in SRS_MODE_FILE.
 * SRS_MODE_FILE uses 4 threads, or 6 when only 1-3 of the available cores are faster than the slowest ones (e.g. 2 big +
 * 6 little), never more than the cores allowed (cpuset / affinity, counting big cores the kernel parks while idle);
 * core speeds from sysfs cpu_capacity, else max frequency; unreadable: 4. Each batch uses only the cores active then. The output never depends on the thread count.
 * Modes (srs_create_mode; output is identical in both): SRS_MODE_FILE (srs_create's default) for processing faster than
 * real time — batches of 32 frames, idle workers spin 50 ms between batches; SRS_MODE_LIVE while recording — batches of
 * 16 frames (output ~0.7 s earlier), idle workers sleep at once; 1-2 threads are enough there (a 4-minute song needs
 * roughly a third of one big core on a Snapdragon 855).
 * NaN / Inf input samples are replaced by 0 (counted by srs_nonfinite_samples) instead of corrupting the rest of the stream.
 * Weight file integrity: a file may end with a 40-byte SHA-256 footer (python/add_checksum.py); when present it is
 * verified at load (any flipped bit, a damaged or cut footer, or stray trailing bytes -> SRS_E_WEIGHTS); files without one
 * load as before. Ship weights with the footer and OR SRS_REQUIRE_CHECKSUM into the mode so a missing footer is refused too.
 *
 * Self-test: srs_selftest() runs the engine on a built-in 1 s test signal (measured 0.3-0.6 s in all on a Snapdragon 855 /
 * Tensor G4, ~3 s on a single Cortex-A55; call it from a worker thread, e.g. once per install and app version, and cache
 * the result). output_check == 1 means the device computes correctly: process while recording. speed of an SRS_MODE_FILE
 * run with threads = 0 tells whether playback from the start right after recording can begin at once (>= ~1.3) or should
 * show progress first. With 2-4 threads the self-test reads 10-50 % low (a short signal), i.e. on the safe side.
 * Recommended flow: create with threads = 0 in SRS_MODE_LIVE when recording starts; a worker pushes the raw recording in
 * order as fast as the engine takes it (keep the raw recording; do not grow an in-memory queue); if the engine falls
 * behind it just lags (backlog = samples recorded - stats.input_samples). When recording ends, srs_set_mode(e,
 * SRS_MODE_FILE, 0), push the rest, srs_flush(); a player can read the output as it grows (output is identical whatever
 * the modes / threads, so the part already processed is kept).
 */
#ifndef SRS_API_H
#define SRS_API_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct srs_engine srs_engine;

enum {
    SRS_OK = 0,
    SRS_E_ARG = -1,          /* bad argument (null pointer, unsupported sample rate, ...) */
    SRS_E_CPU = -2,          /* CPU lacks ARMv8.2 fp16 arithmetic (ARMv8.0 / 8.1 cores such as Cortex-A53 / A73) */
    SRS_E_WEIGHTS = -3,      /* weight file missing or invalid (incl. a checksum footer that does not match) */
    SRS_E_STATE = -4,        /* push after flush, or the engine failed in an earlier push / flush */
    SRS_E_INTERNAL = -5      /* out of memory, threads could not be started, or an internal error */
};

enum { SRS_MODE_FILE = 0, SRS_MODE_LIVE = 1 };
/* OR into the mode of srs_create_* / srs_selftest_*: reject a weight file without a checksum footer (SRS_E_WEIGHTS), so
 * the check cannot be skipped by a file whose footer is missing. Not accepted by srs_set_mode. */
enum { SRS_REQUIRE_CHECKSUM = 0x100 };

#if defined(SRS_EXPORT)
#define SRS_API __attribute__((visibility("default")))
#else
#define SRS_API
#endif

typedef struct {
    double speed;              /* seconds of audio per second of compute (2.0 = twice as fast as real time) */
    double audio_seconds;      /* length of the test signal */
    double compute_seconds;    /* wall time of the timed run (push + flush through a streaming engine) */
    double load_seconds;       /* weight loading and the warm-up run, not part of speed */
    int output_check;          /* 1 = output matches the reference for this weight file; 0 = mismatch (do not enable the
                                  feature on this device); -1 = no reference stored for this weight file */
} srs_selftest_result;

typedef struct {
    long input_samples;        /* pushed so far */
    long output_samples;       /* produced so far (pulled or not) */
    long unpulled_samples;     /* produced, waiting for srs_pull */
    long batches;              /* network passes run */
    double busy_seconds;       /* wall time spent inside srs_push / srs_flush */
    double last_batch_ms;      /* wall time of the latest network pass */
    double max_batch_ms;       /* longest network pass so far (how long one srs_push can block) */
    double recent_load;        /* busy time / audio time over the last ~10 s of input pushed (all input until then); below
                                  1 = faster than real time. 0 before the first push. */
} srs_stats;                   /* layout fixed (callers built against older headers): new fields go to new functions */

SRS_API int srs_cpu_support(void);
SRS_API int srs_create(const char* weights_path, int sample_rate, int threads, srs_engine** out);   /* SRS_MODE_FILE */
SRS_API int srs_create_mode(const char* weights_path, int sample_rate, int threads, int mode, srs_engine** out);
/* The weights from memory (copied: the buffer may be freed after the call) or from an open file descriptor: length bytes at
 * offset (length < 0: to the end of the file), read with pread, so the descriptor's position is neither used nor changed
 * and it is not closed. For an APK asset stored uncompressed: AssetFileDescriptor -> getParcelFileDescriptor().getFd(),
 * getStartOffset(), getLength(); a compressed asset has no descriptor: read it into memory instead. */
SRS_API int srs_create_mem(const void* data, size_t size, int sample_rate, int threads, int mode, srs_engine** out);
SRS_API int srs_create_fd(int fd, long long offset, long long length, int sample_rate, int threads, int mode, srs_engine** out);
SRS_API int srs_push(srs_engine* e, const float* pcm, int n);
SRS_API int srs_pull(srs_engine* e, float* out, int capacity);   /* returns number of samples copied (>= 0) or an error */
SRS_API int srs_flush(srs_engine* e);
SRS_API long srs_latency_samples(const srs_engine* e);
SRS_API long srs_nonfinite_samples(const srs_engine* e);         /* NaN / Inf input samples replaced by 0 so far */
SRS_API int srs_set_mode(srs_engine* e, int mode, int threads);   /* between pushes, before flush; output unaffected;
                                                                    on failure the previous setting stays in force */
SRS_API int srs_get_stats(const srs_engine* e, srs_stats* out);
SRS_API int srs_threads(const srs_engine* e);   /* threads in use now (changes over time with threads = 0 in SRS_MODE_LIVE) */
SRS_API int srs_selftest(const char* weights_path, int sample_rate, int threads, int mode, srs_selftest_result* out);
/* (self-test: threads = 0 means what an engine created with 0 starts with: 1 in SRS_MODE_LIVE, min(cores, 4) in FILE) */
SRS_API int srs_selftest_mem(const void* data, size_t size, int sample_rate, int threads, int mode, srs_selftest_result* out);
SRS_API int srs_selftest_fd(int fd, long long offset, long long length, int sample_rate, int threads, int mode,
                            srs_selftest_result* out);
SRS_API void srs_destroy(srs_engine* e);
SRS_API const char* srs_version(void);

#ifdef __cplusplus
}
#endif
#endif
