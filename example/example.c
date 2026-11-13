// 最小集成示例：读入单声道 float32 原始 PCM，过人声修复引擎，写出同样的 float32 原始 PCM。
// 用法：example <weights.bin> <sample_rate 44100|48000> <in.f32> <out.f32>
// 编译（Android NDK）：
//   $NDK/toolchains/llvm/prebuilt/<host>/bin/aarch64-linux-android21-clang \
//     example.c -I../include -L../lib/arm64-v8a -lvocal_enhancer -o example
// 运行时把 libvocal_enhancer.so 放到可加载路径（APK 的 jniLibs/arm64-v8a，或 adb push 后设 LD_LIBRARY_PATH）。
#include <stdio.h>
#include <stdlib.h>
#include "srs_api.h"

int main(int argc, char** argv) {
    if (argc != 5) { fprintf(stderr, "用法：%s <weights.bin> <44100|48000> <in.f32> <out.f32>\n", argv[0]); return 2; }
    const int sr = atoi(argv[2]);

    const int sup = srs_cpu_support();
    if (sup == 0) { fprintf(stderr, "本机 CPU 不支持（无 ARMv8.2 fp16），应隐藏该功能\n"); return 1; }
    fprintf(stderr, "cpu_support=%d\n", sup);

    srs_engine* e = NULL;
    int rc = srs_create(argv[1], sr, /*threads=*/0, &e);   // 0 = 自动线程
    if (rc != SRS_OK || !e) { fprintf(stderr, "srs_create 失败 rc=%d\n", rc); return 1; }

    FILE* in = fopen(argv[3], "rb");
    FILE* out = fopen(argv[4], "wb");
    if (!in || !out) { fprintf(stderr, "打开文件失败\n"); return 1; }

    float ibuf[4096], obuf[8192];
    size_t n;
    while ((n = fread(ibuf, sizeof(float), 4096, in)) > 0) {
        if (srs_push(e, ibuf, (int)n) != SRS_OK) { fprintf(stderr, "srs_push 失败\n"); return 1; }
        int k;
        while ((k = srs_pull(e, obuf, 8192)) > 0) fwrite(obuf, sizeof(float), (size_t)k, out);
    }
    srs_flush(e);                                           // 收尾：取出尾部（总输出长度 == 总输入长度）
    int k;
    while ((k = srs_pull(e, obuf, 8192)) > 0) fwrite(obuf, sizeof(float), (size_t)k, out);

    fclose(in); fclose(out);
    srs_destroy(e);
    return 0;
}
