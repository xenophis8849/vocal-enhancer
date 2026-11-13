# 最小示例

`example.c`：读入单声道 float32 原始 PCM，过引擎，写出同样格式。演示 `srs_cpu_support` → `srs_create`（自动线程）→ `srs_push` / `srs_pull` → `srs_flush`。

编译（Android NDK，host 换成 linux-x86_64 / darwin-x86_64 等）：
```sh
NDK=/path/to/android-ndk
$NDK/toolchains/llvm/prebuilt/<host>/bin/aarch64-linux-android21-clang \
  example.c -I../include -L../lib/arm64-v8a -lvocal_enhancer -o example
```
运行时需能加载 `lib/arm64-v8a/libvocal_enhancer.so`（放进 APK 的 `jniLibs/arm64-v8a/`，或 `adb push` 后设 `LD_LIBRARY_PATH`），并把 `weights/srs_int8_v1.bin` 作为第一个参数。
