# moonlight-common-c 开发指南

本仓库是用 C11 编写的 GameStream 客户端核心。Sunshine 是发送串流的主机，Moonlight V+ 是接收串流的 Android 应用。三个 Git 仓库在本机的 `D:\project` 下平级。

## 代码该改在哪里

| 仓库 | 职责和入口 |
| --- | --- |
| Sunshine | 采集、编码并发送音视频。`src/rtsp.cpp` 处理主机端协商，`src/stream.cpp` 处理串流收发。主机也直接编译本库的 RTSP 解析器、端点刷新协议等代码，并引用本库定义的 MAT 包格式。[主机开发指南](../sunshine/AGENTS.md) |
| moonlight-common-c | `src/Limelight.h` 是给客户端调用的 C 接口。`src/RtspConnection.c` 负责客户端协商，`src/RtpAudioQueue.c`、`src/RtpVideoQueue.c` 处理音视频接收队列。MAT 包格式在 `src/MatWire.h`，接收和重组在 `src/MatReceiver.c` 与 `src/MatDepacketizer.c`。 |
| Moonlight V+ | 用 Android NDK 把本库和 JNI 桥接代码编入 `libmoonlight-core.so`。应用负责界面、视频解码、音频播放和输入采集。[Android 开发指南](../moonlight-vplus/AGENTS.md) |

本库的 CMake 会收集 `src/` 下的 C 源文件。新增源文件时，按使用方更新显式编译清单，否则公共库单独构建通过后，对应应用仍可能缺少实现。[本库 CMake](CMakeLists.txt) · [Sunshine 编译清单](../sunshine/cmake/sources/host.cmake) · [Moonlight V+ 编译清单](../moonlight-vplus/app/src/main/jni/moonlight-core/Android.mk)

新增协议扩展时，先查对应的客户端标志、主机标志或控制消息编号，再核对发送端写入的字段和接收端读取的字段。[协议扩展编号表](docs/protocol-extension-registry.md)

## 本地开发与 CI 版本

Sunshine 的 `third-party/moonlight-common-c/` 和 Moonlight V+ 的 `app/src/main/jni/moonlight-core/moonlight-common-c/` 都是指向本仓库的 Windows 目录联接。本机改公共代码、查看 `git status` 和提交都在这个平级仓库进行。两个项目下次本地构建时都能读到这里尚未提交的改动。

CI 和普通检出按各自主仓库记录的子模块提交构建。需要升级某一端时，先让 CI 能取到公共库的目标提交，再在该端的普通检出中更新子模块指针。从本仓库根目录可分别查看两个项目锁定的版本。

```bash
git -C ../sunshine rev-parse HEAD:third-party/moonlight-common-c
git -C ../moonlight-vplus rev-parse HEAD:app/src/main/jni/moonlight-core/moonlight-common-c
```

## 独立构建与测试

新检出时先运行 `git submodule update --init --recursive`，取得项目固定的 `enet/` 和用于前向纠错的 `nanors/`。本库依赖所附 ENet 的 API 和 ABI。[依赖说明](README.md)

本机可在 MSYS2 UCRT64 终端使用 CMake、Ninja 和 OpenSSL。从本仓库根目录运行以下命令。静态库构建会启用 MAT 恢复与协商等回归测试。

```bash
cmake -S . -B Build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=ON
cmake --build Build --parallel 4
ctest --test-dir Build --output-on-failure
```

改公共协议时，先运行对应的 CTest，再按上面两份开发指南验证受影响的主机和客户端。涉及 HDMI 输出时，另行记录设备实际播放结果。

## 协作习惯

优先用简单直接的方案。新增校验、兜底或兼容分支时，说清它在实际调用中防止哪个错误。向用户先用白话给出结论，必要时再展开。
