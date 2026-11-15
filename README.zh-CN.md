# 魔法师传奇 HD

[English](README.md) | 简体中文

《魔法师传奇》（*Magic & Mayhem*）的宽屏补丁。它运行原版、未经修改的 `Chaos.exe`，由替换的 `ddraw.dll` 在游戏启动时于内存中打补丁。

- 战场以真正的宽屏分辨率渲染（默认 1920x1080，再缩放到你的显示器）。
- 菜单保持 800x600，等比放大。
- 战斗小地图放大 2 倍，固定在右上角。
- 修复原版在快速战役中出现的 "No place to put wizard" 错误。

本仓库**不包含**游戏本体，你需要有自己的游戏光盘。

## 安装

1. 从[最新发布](https://github.com/vigorousyd168/magic-and-mayhem-hd/releases/latest)下载 zip，解压到一个空文件夹（游戏将安装在这里）。其中包含：
   - `ddraw.dll` 和 `ddraw.ini`：补丁本身
   - `CFG/prefs.cfg`：把帧率设为 32（见下文）
   - `Sounds/Sounds.ini`：修复 `Picview2.wav` 的 `Sound Manger Load Error`
   - `Install from ISO.bat` 和 `install-from-iso.ps1`：从光盘安装游戏

   为避免出现"无法验证发布者"的警告，请先右键单击 zip → 属性 → 勾选**解除锁定** → 确定，然后再解压。
2. 把光盘镜像（`.iso`）拖到 `Install from ISO.bat` 上，或者双击它再选择 `.iso` 文件。也可以把光驱拖上去。

   光盘自带的 `setup.exe` 是 16 位的 InstallShield 安装程序，64 位 Windows 无法运行。这个脚本不经过它，完成同样的安装：解包 `data1.cab`，从光盘复制 `REALMS` 和 `FMV`，并写入 `CDROMDrive.cfg`，之后游戏就不再需要光盘。它不会覆盖已存在的文件，所以补丁里的 `prefs.cfg` 和 `Sounds.ini` 会保留。
3. 运行 `Chaos.exe`。

如果游戏已经装好，也可以直接把 zip 解压到游戏文件夹（`Chaos.exe` 所在的位置）。这时 `CFG/prefs.cfg` 和 `Sounds/Sounds.ini` 会替换游戏原有的文件，如果你改过设置，请先备份 `CFG/prefs.cfg`。如果游戏仍然要求插入光盘，把 `CDROMDrive.cfg` 改成 `CDROMDrive=.\`，并把光盘上的 `REALMS` 和 `FMV` 复制到游戏文件夹。

## 设置（`ddraw.ini`）

| 键 | 含义 |
| --- | --- |
| `width`, `height` | 窗口客户区大小。`0 0` = 占满整个显示器的无边框全屏。 |
| `renderw`, `renderh` | 游戏渲染的尺寸，之后再缩放。越小，界面和文字越大，屏幕上显示的地图越少。在 1440p 显示器上，`1920 1080` = 界面放大 1.33 倍，`1280 720` = 界面放大 2 倍，`0 0` = 原生分辨率。 |
| `log`, `dump`, `script` | 调试用，保持为 0。 |

## 游戏速度

战斗逻辑与帧率绑定，所以 `CFG/prefs.cfg` 里的 `MaxFramesPerSec` 决定游戏速度。附带的 `prefs.cfg` 设为 32。游戏内的"参数选择"界面只提供 14/17/20，在那里修改会覆盖这个值。

## 已知限制

- 过场动画已关闭（`PlayFMV=FALSE`）：动画使用 Indeo 4/5 编码，Windows 11 上没有解码器。
- 没有背景音乐：游戏的音乐是 CD 音轨。
- 尚未测试：无边框全屏、施放法术、第 2 个区域开头之后的战役，以及 100x100 地图上的小地图。

## 编译

DLL 使用 32 位 MinGW 工具链编译（例如 llvm-mingw）：

```sh
CC=/path/to/i686-w64-mingw32-gcc sh src/ddraw/build.sh
```

输出为 `game/ddraw.dll`。

`src/ddraw/patches.h` 由 `tools/gen_patches.py` 根据 `Chaos.exe` 的 capstone 反汇编清单生成：

```sh
python tools/gen_patches.py chaos_listing.txt src/ddraw/patches.h
```

## 目录结构

- `src/ddraw/ddraw.c`：DirectDraw 包装层和内存补丁
- `src/ddraw/patches.h`：生成的屏幕尺寸立即数表
- `tools/gen_patches.py`：`patches.h` 的生成器
- `game/`：编译好的 DLL、修改过的配置文件和从 ISO 安装的脚本

## 许可证

补丁代码使用 [MIT 许可证](LICENSE)。《魔法师传奇》及其游戏文件归各自的权利人所有，不包含在本仓库中。
