package com.vocalenhancer;

/**
 * SRS vocal restoration, streaming. One instance per stream; calls block while computing. Methods are synchronized, so
 * close() from another thread waits for a running push() instead of freeing the engine under it.
 * Modes (same output): MODE_FILE for faster-than-real-time processing, MODE_LIVE while recording (idle threads sleep
 * between batches; output ~0.7 s earlier). threads = 0 means automatic: MODE_LIVE starts with 1 thread and adds or
 * drops threads to stay just faster than real time (at most 4); MODE_FILE uses up to 4.
 * Mono float PCM in [-1, 1] at 44100 or 48000 Hz in, same rate and same total length out (peak <= -0.3 dBFS).
 * Output starts ~7 s after input starts; the tail arrives after flush().
 * push() returns in ~1 ms most of the time but computes a whole batch (~1.4 s of audio) about once every 15 calls
 * (up to ~0.3 s on a Snapdragon 855): call it from a worker thread, never from the recording callback.
 *
 * Recommended flow: create with threads = 0 in MODE_LIVE when recording starts and push the raw recording from a worker
 * thread as fast as the engine takes it (if it falls behind it just lags; keep the raw recording). When recording ends,
 * setMode(MODE_FILE, 0), push the rest and flush(); a player can play the output as it grows. Run selfTest(...) once per
 * install / app version: outputCheck == 1 to enable processing; speed of a MODE_FILE run with 0 threads >= ~1.3 means
 * playback can start at once after recording.
 *
 * Weights: a file path, an open descriptor (an APK asset stored uncompressed — add the extension to noCompress — via
 * AssetFileDescriptor afd = assets.openFd("srs_int8_v1.bin"):
 * new SrsEngine(afd.getParcelFileDescriptor().getFd(), afd.getStartOffset(), afd.getLength(), 44100, 4, MODE_FILE);
 * the descriptor is read with pread and not closed: close afd afterwards), or the bytes (e.g. a compressed asset read
 * fully; copied, ~11 MB). A file with a checksum footer is verified at load.
 *
 *   if (SrsEngine.cpuSupport() == 0) hideFeature();
 *   SrsEngine e = new SrsEngine(weightsPath, 44100, 4);
 *   e.push(pcm, n); int k = e.pull(buf);  ...  e.flush(); while ((k = e.pull(buf)) > 0) ...;  e.close();
 */
public final class SrsEngine implements AutoCloseable {
    static { System.loadLibrary("vocal_enhancer"); }

    /** 0 = unsupported CPU, 1 = int8 dot-product, 2 = int8 matrix-multiply (i8mm), 3 = fp16 only (no int8 dot product:
     *  slower; decide record-while-processing with selfTest as on any device). */
    public static native int cpuSupport();
    public static native String version();

    public static final int MODE_FILE = 0, MODE_LIVE = 1;
    /** OR into the mode when creating or self-testing: reject a weight file that has no checksum footer. */
    public static final int REQUIRE_CHECKSUM = 0x100;

    private long handle;

    /** Result of {@link #selfTest}. */
    public static final class SelfTest {
        /** Seconds of audio per second of compute (2.0 = twice as fast as real time). */
        public final double speed, audioSeconds, computeSeconds, loadSeconds;
        /** 1 = output matches the reference; 0 = mismatch (do not enable on this device); -1 = no reference for this weight file. */
        public final int outputCheck;
        SelfTest(double[] v) { speed = v[0]; audioSeconds = v[1]; computeSeconds = v[2]; loadSeconds = v[3]; outputCheck = (int) v[4]; }
    }

    /** Snapshot of {@link #stats}. */
    public static final class Stats {
        public final long inputSamples, outputSamples, unpulledSamples, batches;
        /** Threads in use now (changes over time when the engine was created with threads = 0 in MODE_LIVE). */
        public final int threads;
        /** busySeconds: wall time inside push / flush; maxBatchMs: longest single network pass (how long push can block). */
        public final double busySeconds, lastBatchMs, maxBatchMs;
        /** Compute time / audio time over the last ~10 s pushed; below 1 = faster than real time. */
        public final double recentLoad;
        Stats(long[] l, double[] d) {
            inputSamples = l[0]; outputSamples = l[1]; unpulledSamples = l[2]; batches = l[3]; threads = (int) l[4];
            busySeconds = d[0]; lastBatchMs = d[1]; maxBatchMs = d[2]; recentLoad = d[3];
        }
    }

    /**
     * Runs the engine on a built-in 1 s test signal with these threads / mode and checks its output (blocking: measured
     * 0.3-0.6 s on a Snapdragon 855 / Tensor G4, ~3 s on a single Cortex-A55). Reads 10-50 % low with 2-4 threads (safe
     * side). Throws if the CPU is unsupported or the weight file is bad.
     */
    public static SelfTest selfTest(String weightsPath, int sampleRate, int threads, int mode) {
        double[] v = new double[5];
        check(nativeSelfTest(weightsPath, sampleRate, threads, mode, v));
        return new SelfTest(v);
    }

    /** Self-test with the weights from an open descriptor (see the class comment). */
    public static SelfTest selfTest(int fd, long offset, long length, int sampleRate, int threads, int mode) {
        double[] v = new double[5];
        check(nativeSelfTestFd(fd, offset, length, sampleRate, threads, mode, v));
        return new SelfTest(v);
    }

    /** Self-test with the weights as bytes. */
    public static SelfTest selfTest(byte[] weights, int sampleRate, int threads, int mode) {
        double[] v = new double[5];
        check(nativeSelfTestMem(weights, sampleRate, threads, mode, v));
        return new SelfTest(v);
    }

    public SrsEngine(String weightsPath, int sampleRate, int threads) { this(weightsPath, sampleRate, threads, MODE_FILE); }

    /** Weights from length bytes at offset of an open descriptor (length < 0: to the end); the descriptor is not closed. */
    public SrsEngine(int fd, long offset, long length, int sampleRate, int threads, int mode) {
        long[] h = new long[1];
        int rc = nativeCreateFd(fd, offset, length, sampleRate, threads, mode, h);
        if (rc != 0) throw new IllegalStateException("srs_create_fd failed: " + rc);
        handle = h[0];
    }

    /** Weights as bytes (copied; the array can be dropped afterwards). */
    public SrsEngine(byte[] weights, int sampleRate, int threads, int mode) {
        long[] h = new long[1];
        int rc = nativeCreateMem(weights, sampleRate, threads, mode, h);
        if (rc != 0) throw new IllegalStateException("srs_create_mem failed: " + rc);
        handle = h[0];
    }

    public SrsEngine(String weightsPath, int sampleRate, int threads, int mode) {
        long[] h = new long[1];
        int rc = nativeCreate(weightsPath, sampleRate, threads, mode, h);
        if (rc != 0) throw new IllegalStateException("srs_create failed: " + rc);
        handle = h[0];
    }

    public synchronized void push(float[] pcm, int n) { check(nativePush(handle, pcm, n)); }
    /** Copies up to buf.length available output samples into buf; returns the count (0 when nothing is ready). */
    public synchronized int pull(float[] buf) { int k = nativePull(handle, buf); check(Math.min(k, 0)); return k; }
    public synchronized void flush() { check(nativeFlush(handle)); }
    public synchronized long latencySamples() { return nativeLatency(handle); }
    /** NaN / Inf input samples that were replaced by 0 so far. */
    public synchronized long nonfiniteSamples() { return nativeNonfinite(handle); }
    /** Switch mode / threads between pushes (before flush); the output does not change. */
    public synchronized void setMode(int mode, int threads) { check(nativeSetMode(handle, mode, threads)); }
    public synchronized Stats stats() {
        long[] l = new long[5]; double[] d = new double[4];
        check(nativeStats(handle, l, d));
        return new Stats(l, d);
    }

    @Override public synchronized void close() { if (handle != 0) { nativeDestroy(handle); handle = 0; } }

    private static void check(int rc) { if (rc < 0) throw new IllegalStateException("srs error " + rc); }

    private static native int nativeCreate(String path, int sr, int threads, int mode, long[] out);
    private static native int nativeCreateFd(int fd, long off, long len, int sr, int threads, int mode, long[] out);
    private static native int nativeCreateMem(byte[] data, int sr, int threads, int mode, long[] out);
    private static native int nativeSelfTestFd(int fd, long off, long len, int sr, int threads, int mode, double[] out);
    private static native int nativeSelfTestMem(byte[] data, int sr, int threads, int mode, double[] out);
    private static native int nativePush(long h, float[] pcm, int n);
    private static native int nativePull(long h, float[] buf);
    private static native int nativeFlush(long h);
    private static native long nativeLatency(long h);
    private static native long nativeNonfinite(long h);
    private static native int nativeSetMode(long h, int mode, int threads);
    private static native int nativeStats(long h, long[] l, double[] d);
    private static native int nativeSelfTest(String path, int sr, int threads, int mode, double[] out);
    private static native void nativeDestroy(long h);
}
