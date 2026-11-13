# vocal-enhancer

安卓端上人声修复 SDK：把 [Smule Renaissance Small](https://github.com/smulelabs/smule-renaissance)（SRS，MIT）落到 arm64 安卓，手写 C++ 流式推理，无需推理框架。本仓库只提供**接口与预编译库**；实现闭源。

- 库 `lib/arm64-v8a/libvocal_enhancer.so`（约 0.6 MB，Android API 21+，arm64-v8a）
- 权重 `weights/srs_int8_v1.bin`（8 bit 按通道量化，约 11 MB，尾部带 SHA-256 校验）
- C 接口 `include/srs_api.h`，Java 封装 `java/com/vocalenhancer/SrsEngine.java`
- 许可：本封装与接口 Apache-2.0（见 `LICENSE`）；第三方组件见 `THIRD_PARTY_NOTICES.md`

## 能力与机型

| 项 | 说明 |
|---|---|
| 输入 / 输出 | 单声道 float PCM，[-1, 1]，44100 或 48000 Hz 原生处理；输出同采样率、同总长度、峰值 ≤ −0.3 dBFS |
| 支持的 CPU | arm64-v8a，CPU 有 ARMv8.2 的 fp16 运算（Cortex-A55 / A75 及以后）。`srs_cpu_support()`：0 = 不支持（请隐藏该功能），1 = int8 dotprod，2 = int8 i8mm，3 = 仅 fp16（无 int8 点积，较慢，输出与 int8 略有不同但已验收等价） |
| 不支持 | 只有 A53 / A73 等 ARMv8.0/8.1 核心的芯片返回 0（能加载、不崩溃，但不应启用） |
| 延迟 | 模型向后看约 5.5 s，输出比输入晚约 7 s 开始；收尾在 `srs_flush()` |

## 用法（C 接口）

```c
#include "srs_api.h"

if (srs_cpu_support() == 0) { /* 隐藏功能 */ }

srs_engine* e = NULL;
int rc = srs_create("srs_int8_v1.bin", 44100, /*threads=*/0, &e);   // threads=0：自动
if (rc != SRS_OK) { /* 处理错误码，见 srs_api.h */ }

// 工作线程里（不要在录音回调里调用）：
srs_push(e, pcm, n);
int k; while ((k = srs_pull(e, buf, cap)) > 0) consume(buf, k);
// 结束：
srs_flush(e); while ((k = srs_pull(e, buf, cap)) > 0) consume(buf, k);   // 总输出长度 == 总输入长度
srs_destroy(e);
```

一路流一个引擎，同一引擎只在一个线程里调用。权重也可从内存或文件描述符加载（`srs_create_mem` / `srs_create_fd`，见头文件）。`example/` 有可编译的最小例子。Java 侧见 `SrsEngine.java`。

## 推荐的产品流程（录完进预览页回放）

1. 开录时 `srs_create_fd(…, threads=0, SRS_MODE_LIVE)`；工作线程从原始录音按顺序取数推入，能推多快推多快（原始录音始终保存）。不要在录音回调里调用引擎——把 PCM 放进环形缓冲，由工作线程推。
2. 录音时引擎按负载自动加减线程，只求跟上实时；跟不上就让它落后，不额外抢核。
3. 录完进预览页：同一引擎 `srs_set_mode(e, SRS_MODE_FILE, 0)`，把剩下的推完再 `srs_flush`。已处理部分不重算，结果与全程实时处理逐位相同。
4. 预览页播放器从「已处理好的部分」读，边产出边追加。设录音时速度 r、补算速度 S（相对实时），从头播放不卡的条件是 r + S ≥ 1（留余量取 1.3）。

硬件下限：CPU 有 fp16 运算、且能用到 ≥2 个 A55 级核心。慢机录音侧缓冲建议留 ≥2 s。

## 线程（`threads = 0` 自动）

文件模式自动线程数按「允许用的核」的排布定：比最慢档快的核有 1–3 个（如 2 大 + 6 小）用 6 线程，否则 4，不超过允许核数。录音模式从 1 线程起、按实测负载增减，上限同上。输出不随线程数变化。

## 实测性能（整首，文件模式，冷机）

| 机型 | 48 kHz（4 分 22 秒） | 44.1 kHz（4 分 45 秒） | 边录边算平均占核 |
|---|---|---|---|
| Galaxy S10（骁龙 855，dotprod） | 约 11× | 约 10.5× | 约 0.35–0.44 核 |
| Pixel 9（Tensor G4，i8mm） | 约 19–21× | 约 18× | 约 0.34–0.52 核 |
| OPPO（骁龙 710，仅 fp16，6 线程） | 约 4.3× | — | 约 0.83–0.96 核 |

处理峰值内存约 43–51 MB（fp16 路径略高）；加载（含 SHA-256 校验）大核约 0.1–0.2 s。

## 稳定性

CPU 能力检测、自测（`srs_selftest*`，输出指纹核对）、运行统计（`srs_get_stats`）、中途切换模式、从内存 / 文件描述符加载、SHA-256 权重校验、NaN/Inf 防护。错误码与边界见 `srs_api.h`。

## 目录

```
include/srs_api.h                         C 接口
java/com/vocalenhancer/SrsEngine.java     Java 封装
lib/arm64-v8a/libvocal_enhancer.so        预编译库
weights/srs_int8_v1.bin                   权重（带 SHA-256 校验）
example/                                  最小集成示例
LICENSE                                   Apache-2.0
THIRD_PARTY_NOTICES.md                    第三方许可
```
