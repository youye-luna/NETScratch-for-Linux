# NETScratch for Linux
一个普通的多线程局域网 IP 扫描工具

## 软件由来
作者闲着没事干用 AI 做出来的东西<br>
（作者 PS：其实网上有很多比我这个做的更好的工具了，我做这个纯属白搭）

## 支持系统
已适配四大发行版家族与主流桌面环境：

| 发行版家族 | 代表发行版 |
| --- | --- |
| Debian 系 | Debian、Ubuntu、Linux Mint、Deepin、Kali |
| 红帽系 | RHEL、CentOS、Rocky、AlmaLinux、Fedora、openEuler |

- 桌面环境：GNOME、KDE Plasma、XFCE、MATE、Cinnamon、LXQt 等（X11 会话，或在 Wayland 会话下经 XWayland 运行）。
- 界面字体自动从系统已安装的中文字体中挑选（Noto Sans CJK / 思源黑体 / 文泉驿等），并按需回退。

## 依赖
### 编译依赖
```bash
# Debian 系
sudo apt install build-essential cmake qtbase5-dev qtbase5-dev-tools
# 可选：更多图片格式（ico/jpeg/tiff/webp 等）与 GTK 原生主题
sudo apt install qt5-image-formats-plugins qt5-gtk-platformtheme

# 红帽系（Fedora / RHEL / Rocky / Alma / openEuler）
sudo dnf install gcc-c++ cmake qt5-qtbase-devel
# 可选：GTK 主题与更多图片格式
sudo dnf install qt5-qtstyleplugins qt5-qtsvg

# Arch 系
sudo pacman -S base-devel cmake qt5-base
# 可选
sudo pacman -S qt5-svg

# SUSE 系
sudo zypper install gcc-c++ cmake libqt5-qtbase-devel
# 可选
sudo zypper install libqt5-qtsvg-devel
```

### 运行依赖
| 用途 | 程序 | 所在软件包（Debian / 红帽 / Arch / SUSE） |
| --- | --- | --- |
| 主机发现 | `nmap` | `nmap` / `nmap` / `nmap` / `nmap` |
| ICMP 存活探测 | `ping` | `iputils-ping` / `iputils` / `iputils` / `iputils` |
| IPv6 邻居 / 网关读取 | `ip` | `iproute2` / `iproute2` / `iproute2` / `iproute2` |

> 未安装 nmap 时主机发现功能不可用，程序会在界面提示；
> 非 root 运行时 nmap 无法拿到 MAC 地址，程序会改用系统 ping + `/proc/net/arp` 补足。

## 构建与部署
用户要求：只产出包含全部运行库与可执行文件的 `release` 目录，不打包 zip / 安装器。

```bash
# 1. 配置
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

# 2. 编译
cmake --build build -j"$(nproc)"

# 3. 部署到 release/（可执行文件 + Qt 运行库 + Qt 插件 + 系统依赖库 + qt.conf）
cmake --build build --target deploy

# 4. 运行
cd release && ./NETScratch
```

`release/` 目录内容示例：

```
release/
├── NETScratch              # 可执行文件（RPATH=$ORIGIN）
├── qt.conf                 # 告知 Qt 库/插件就在本目录
├── libQt5Core.so.5         # Qt 运行库
├── libQt5Gui.so.5
├── libQt5Widgets.so.5
├── libQt5Network.so.5
├── libQt5XcbQpa.so.5
├── libicudata.so.* ...     # Qt 依赖的第三方库
├── platforms/libqxcb.so    # X11 / XWayland 平台插件（必需）
├── platformthemes/libqgtk3.so
├── styles/libqgtk3.so
├── imageformats/           # ico/jpeg/gif/svg/...
├── iconengines/
└── xcbglintegrations/
```

整个 `release/` 目录可直接拷贝到另一台同架构 Linux 上运行，无需安装 Qt。
glibc、libstdc++、libgcc_s 以及显卡/驱动相关库（libGL / libEGL / libdrm / NVIDIA 等）
刻意不打包，使用目标系统自带版本，以保证跨发行版兼容。

> `release/` 内全部为实体文件（不含符号链接），可安全地在 Windows / U 盘 / 网络间拷贝。
> 从 Windows 等不保留执行位的文件系统拷回 Linux 后，需补一次可执行权限：
>
> ```bash
> chmod +x release/NETScratch
> ```

> 若目标机是 Wayland 会话，程序会经由 XWayland 运行（GNOME/KDE 默认启用 XWayland）。
> 若系统未安装 XWayland，可执行 `sudo apt install xwayland`（对应各发行版包名安装）。

## 生成安装包（deb / rpm）
在 `release/` 基础上可进一步产出 `.deb`（Debian 系）与 `.rpm`（红帽 / SUSE 系）安装包。
安装包同样**自包含**：把 `release/` 里的全部 Qt 运行库与插件一并装入 `/opt/NETScratch`，
运行期只需系统提供 `nmap` / `ping` / `ip` 三个外部命令。

```bash
# 生成 .deb（自动先执行 deploy 生成 release/）
cmake --build build --target package_deb

# 生成 .rpm
cmake --build build --target package_rpm

# 产物输出到源码目录下的 packages/
ls packages/
```

安装 / 卸载：

```bash
# Debian / Ubuntu / Deepin / Kali ...
sudo apt install ./packages/netscratch_*_amd64.deb

# 红帽系 / openSUSE ...
sudo dnf install ./packages/netscratch-*.rpm      # 红帽系
sudo zypper install ./packages/netscratch-*.rpm   # SUSE 系
```

安装后从应用菜单启动「NETScratch」，或命令行执行 `netscratch`。包内布局：

```
/opt/NETScratch/                     # 可执行文件 + 全部 Qt 运行库与插件
/usr/bin/netscratch                  # 启动器
/usr/share/applications/netscratch.desktop
/usr/share/icons/hicolor/256x256/apps/netscratch.png
```

> 打包前请先执行一次 `cmake --build build --target deploy`（`package_deb` / `package_rpm` 会自动触发），
> 安装包内容即取自 `release/` 目录。

## 软件功能
- IP 占用扫描（主机发现由 [nmap](https://nmap.org/) 子进程完成）
- DHCP 服务器扫描
- 扫描历史记录
- （后面再加）

## 开源协议
本项目使用 **GNU General Public License v3.0** 开源，完整协议见 [LICENSE](LICENSE)。<br>
主机发现调用系统安装的官方 nmap，nmap 使用 NPSL 许可（与 GPLv3 不兼容），
不随本程序分发，请在目标机上自行安装，版权归 nmap 项目所有。

## 软件截图（Windows 版界面，Linux 版布局一致）
演示系统：Debian 13.6.0+GNOME 48和Rocky 9+KDE 5.27.12
Debian+GNOME
<img width="1720" height="952" alt="图片" src="https://github.com/user-attachments/assets/059e2d62-395f-4b5e-9743-d49990f3c6ca" />
<img width="1720" height="952" alt="图片" src="https://github.com/user-attachments/assets/a33c434b-d4dd-4acd-bf2f-ca3b1e39a0e0" />
<img width="1720" height="952" alt="图片" src="https://github.com/user-attachments/assets/14df0e98-29f1-4045-8b0a-242dde7d984e" />
Rocky+KDE
<img width="1720" height="952" alt="图片" src="https://github.com/user-attachments/assets/9676fa99-6ed6-4a04-a632-00adfe371d1f" />
<img width="1720" height="952" alt="图片" src="https://github.com/user-attachments/assets/00df783b-5004-4ef5-8f7f-0675570cce3a" />
<img width="1720" height="952" alt="图片" src="https://github.com/user-attachments/assets/882d0074-0163-4dab-a0de-6f7360fe1a23" />


## 结尾

有建议或 bug：提 issues
不喜勿喷<br>
本人是真的不太会编程
