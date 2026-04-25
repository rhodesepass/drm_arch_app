![app_banner](./docs/assets/drm_app_banner.png)

# drm_arch_app 电子通行证播放器程序

当前维护入口与运行时名称统一为 `drm_arch_app`。

基于DRM、LVGL和一点寄存器魔法的“方舟通行证”展示程序

```mermaid
graph TD
    %% ===== Top -> Bottom blend order: 2 / 1 / 0 =====
    
    subgraph MGMT["Managements"]
        PRTS["PRTS 调度/定时器<br/>src/prts/*"]
        APP["拓展APP/IPC<br/>src/apps/*"]
        
        APP --> PRTS
    end

    subgraph L2["Layer 2 / UI (RGB565) - Top"]
        LVGL["LVGL 渲染与线程<br/>src/render/lvgl_drm_warp.c"]
        EEZ["EEZ Studio 生成 UI<br/> generated_ui/*"]
        ACTIONS["业务回调/逻辑<br/>src/ui/actions_*.c<br/>src/ui/scr_transition.c 等"]
        EEZ --> LVGL
        ACTIONS --> LVGL
    end

    subgraph L1["Layer 1 / Overlay - Middle"]
        OVERLAY["Overlay 绘制<br/>transitions/opinfo<br/>src/overlay/*"]
        
        PRTS --> OVERLAY
    end

    subgraph L0["Layer 0 / Video - Bottom"]
        MP["Mediaplayer (Cedar 解码)<br/>src/render/mediaplayer.c"]
        PRTS --> MP
    end

    subgraph DW["Driver: drm_warpper + custom ioctl"]
        Q["display_queue/free_queue + display thread<br/>等待 vblank -> ioctl commit"]
        IOCTL["DRM_IOCTL_SRGN_ATOMIC_COMMIT<br/>挂载 FB / 设置坐标 / 设置 alpha"]
        DEBE["sun4i DEBE<br/>Plane Blend: 2/1/0"]
        Q --> IOCTL --> DEBE
    end
    
    subgraph UTILS["Utils"]
        LOG["日志"]
        BQ["队列"]
        T["定时器"]
        MISC["....."]
    end

    LVGL -->|"enqueue NORMAL FB"| Q
    OVERLAY -->|"enqueue NORMAL FB"| Q
    MP -->|"enqueue YUV FB"| Q
```

## 开始使用

播放程序与电子通行证固件一起分发，可在[https://github.com/rhodesepass/buildroot/releases](https://github.com/rhodesepass/buildroot/releases)下载最新固件。

## 编译方法

### 直接编译

需要提前准备的其他源码：

* 本项目的buildroot https://github.com/rhodesepass/buildroot

构建:

1. 拉取上文提到的buildroot,按repo中readme编译一次 编译工具链及依赖库
2. 运行source ./output/host/environment-setup 将生成的工具链和依赖设置为默认工具链和依赖
3. 在本repo目录下:
```bash
mkdir build
cd build
cmake .. # 以release模式编译，或
cmake -DCMAKE_BUILD_TYPE=Debug .. # 以debug模式编译
make
```

若正常则终端显示此日志且 `build` 目录中出现 `drm_arch_app` 二进制文件

```
[100%] Built target drm_arch_app
```

### 跟随buildroot一起生成

直接拉取上文提到的 buildroot，按 repo 中 readme 编译一次，生成的镜像里就有了。Buildroot 编译目录位于 `output/build/drm_arch_app-local/`。

## 开发指引

* [程序结构](docs/application_structure.md)
* [overlay层开发指南](docs/overlay_dev_note.md)

## 目录约定

- `generated_ui/`：当前参与编译的 EEZ 导出代码
- `../ui_design/epass_eez/`：EEZ Studio 工程与导出辅助工具
- `../third_party/lvgl/`：共享 LVGL 依赖源码

## 直接嵌入的开源代码

* [log](https://github.com/rxi/log.c) A minimal but powerful logging facility for C.
* [stb](https://github.com/nothings/stb) single-file public domain libraries for C/C++
* [code128](https://github.com/fhunleth/code128) barcode generator
* [lvgl](https://github.com/lvgl/lvgl) Embedded graphics library to create beautiful UIs for any MCU, MPU and display type.
* [cJSON](https://github.com/DaveGamble/cJSON) Ultralightweight JSON parser in ANSI C
