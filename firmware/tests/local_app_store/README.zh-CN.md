# 本机应用市场测试服务

这个目录集中保存本机 HTTP 测试服务、封面转换工具、配置和验证代码。
**`packages` 就是允许通过 HTTP 提供下载的应用固件目录，由你手动放入固件。**

## 启动

把编译好的 `voice.bin` 等兼容应用固件手动复制到 `packages`。
文件名可以修改，服务从固件内部读取应用 ID、名称和版本，并计算大小和 SHA-256。
不需要 `package.json`，同一个应用 ID 只保留一份。

在本目录运行：

```powershell
python -m pip install -r requirements.txt
python server.py
```

也可以从任意目录用脚本的完整路径启动。例如：

```powershell
python E:\Deotaland\Agent_link\firmware\tests\local_app_store\server.py
```

默认始终读取本目录的 `packages`，不受 PowerShell 当前目录影响。
`--bundle`、`--covers-config` 和封面转换命令的相对路径也均相对于本目录。
空目录可以正常启动，在线应用列表为空。factory、bootloader、merged 等不能作为应用
安装的固件会跳过并打印原因；不会把整机包当作应用下发。

设备与电脑连接同一局域网，在设备的应用服务设置中填写启动时打印的地址：
`http://电脑局域网IP:8765/api/apps`。
新增、替换、删除固件或修改封面后重新启动服务。服务运行期间替换固件，会暂时返回
HTTP 409，避免用旧校验值提供新文件。

## 封面

`covers.json` 按应用 ID 配置原图，`source_file` 相对于配置文件所在目录。
Voice 默认使用 `assets/voice.png`；本地文件不存在时，使用原来的图片 URL。
无需生成新图。原有 132×132 RGB565LE+A8 协议保持不变。
转换后的封面保存在 `.cache/covers`，不混入手动放固件的 `packages`。

需要单独转换时，在本目录运行：

```powershell
python make_app_cover.py assets/voice.png --app-id agent.voice --output assets/voice.raw
```

## 构建与验证

已有固件时，HTTP 服务不需要 ESP-IDF。需要从源码重新构建时，在激活 ESP-IDF 的终端运行：

```powershell
python ../../tools/package_firmware.py waveshare-amoled-175-wifi --public --jobs 2
```

统一使用正式的 `firmware/tools/package_firmware.py`，`--public` 检查共享包不含私有构建配置。
构建仍复用 `firmware/build`，产物在 `firmware/build/packages/waveshare-amoled-175-wifi/`。
构建工具不会自动发布到 `packages`；只把你需要提供下载的应用 `.bin` 手动复制进去。
完整烧录包的说明见[板级 factory 文档](../../factory/waveshare-amoled-175/README.md)。

本机接口和路径验证：

```powershell
python tests/test_server.py
```

测试会使用临时文件和本机临时端口，结束后关闭服务。不会打开设备串口或烧录固件。
