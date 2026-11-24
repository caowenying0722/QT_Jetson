# Jetson 人脸门禁第一版

主控上的工程目录是 `/home/nv1/smartlock_demo`。应用用 C++17、OpenCV DNN 和 Qt Widgets。Caffe SSD 找人脸框，OpenFace 把框内的图像变成 128 维特征。CLI 和 Qt 窗口共用同一个识别服务。模拟开锁以界面日志或 CLI 的 `simulated_unlock` 字段呈现。

## 1. 确认模型可用

在 Jetson 的终端执行。Windows 端可以先运行 `ssh wheeltec-car`。

```bash
cd ~/smartlock_demo
(cd assets && sha256sum -c SHA256SUMS)
./build-stable/smartlock model-check
```

看到 `Caffe detector and OpenFace recognizer loaded on CPU` 即完成这一步。三个模型文件分别是检测网络结构、检测权重和识别权重。先检查这些文件，可以在运行界面前发现模型缺失或传输损坏。

想看代码时，从 [`FaceService::FaceService`](src/face_service.cpp) 开始，跟踪三个文件路径如何传给 OpenCV，以及两个网络在哪里选择 CPU 后端。实机已编译好。如果改了源码，在工程目录执行：

```bash
cmake -S . -B build-stable -DCMAKE_BUILD_TYPE=Release
cmake --build build-stable --parallel 2
```

## 2. 在命令行完成录入与验证

在同一个终端依次运行。每次设置新的 `lesson_db` 都会得到一套空的练习数据。

```bash
lesson_db="$(mktemp -d)/faces.json"
./build-stable/smartlock --db "$lesson_db" enroll '样例 A' assets/samples/enroll.jpg
./build-stable/smartlock --db "$lesson_db" check assets/samples/known.jpg
./build-stable/smartlock --db "$lesson_db" check assets/samples/unknown.jpg
echo "$?"
```

| 图片 | 实机参考结果 |
|---|---|
| `enroll.jpg` | 录入成功，保存样例 A 的特征 |
| `known.jpg` | 同一个人的另一张照片，相似度约 0.746，授权，`simulated_unlock=true` |
| `unknown.jpg` | 另一人，相似度约 0.595，拒绝，`simulated_unlock=false`，退出码为 3 |

`enroll` 先确认只有一张脸，再计算并保存特征。同名录入会提示已存在。模板库保存名称和单位长度的特征向量，文件权限为 0600。录入的写锁覆盖读取到保存的整个过程，`QSaveFile` 通过原子替换提交更新。新的 CLI 进程可以立即读到完整记录。

在 [`FaceService::check`](src/face_service.cpp) 跟踪一次验证。检测得到人脸框，`embed()` 把裁剪图变成单位向量，随后计算与库中各向量的点积。单位向量的点积就是余弦相似度。最高分达到 0.65 才授权。没有人脸、多人入镜和空库都有单独的拒绝状态。0.65 是本版的演示阈值，换成实际场景的照片后需重新评估。

可以把 `check` 的文件路径换成自己的照片。验证流程的计时从图片已经解码为 `cv::Mat` 后开始，包含检测、特征计算与匹配，另含本次模板读取。

## 3. 在 Qt 界面复现

在 Jetson **远程桌面内的终端**执行。练习库与上一节独立，因此可以从空库重做录入。

```bash
cd ~/smartlock_demo
./build-stable/smartlock_gui --db data/ui-lesson.json assets/samples/enroll.jpg
```

1. 在「录入名称」输入 `样例 A`，点「录入当前图片」。预览会画出检测框，日志会显示录入结果。
2. 点「选择图片…」，打开 `assets/samples/known.jpg`，再点「验证当前图片」。状态显示「授权通过 · 模拟开锁」，会话记录增加一条开锁事件。
3. 换成 `unknown.jpg` 再验证。状态显示「拒绝 · 未知用户」，开锁事件仍只有一条。

如果已经做过这组练习，可以改用一个新的 `--db` 文件名复现空库。省略 `--db` 时，CLI 与 GUI 都使用 `data/faces.json`。

界面从 [`gui_main.cpp`](src/gui_main.cpp) 进入。读 [`DemoWindow::check`](src/demo_window.cpp) 时，找出按钮连接的函数、状态文字从哪里生成，以及哪一行写入模拟开锁事件。图片预览转换成 Qt 持有的像素副本，临时的 OpenCV 图像释放后窗口仍可正常绘制。

## 4. 复跑验收

```bash
cd ~/smartlock_demo/build-stable
ctest -V --output-on-failure
```

`cli_flow` 覆盖模型加载、空库拒绝、独立进程读回模板、已知授权、未知拒绝和重复录入。 `gui_smoke` 在离屏 Qt 中触发实际按钮，核对授权只产生一次开锁事件、未知人与空白图均被拒绝。另在 Jetson 上用已有录制中的 `frame_0002412.jpg` 做过一次只读回放，结果为 `no_face`。

## 5. 在 Qt 中使用 USB 摄像头

先在 Jetson 终端确认设备：

```bash
cam=/dev/v4l/by-id/usb-Generic_USB2.0_PC_CAMERA-video-index0
ls -l "$cam"
udevadm info --query=property --name="$cam" | grep -E 'ID_(MODEL|USB_DRIVER|V4L_CAPABILITIES)='
```

实机识别为 `USB2.0_PC_CAMERA`，驱动是 `uvcvideo`，当前采集节点为 `/dev/video2`。设备编号会随插拔变化，界面使用 `by-id` 路径。

这只 USB2.0 摄像头最高为 640×480 YUYV。试探请求 800×600、1024×768、720p 或 1080p 时，驱动都返回 640×480。现场试过将锐度从默认值 3 调到 6，再调到 8，同时将背光补偿从 1 调到 2。V4L2 未暴露曝光或对焦控制。用正面照明，在 30、50、80 厘米三档试距离，选眼鼻轮廓清楚的一帧再录入。

如果远程桌面已有窗口，直接使用它。在 Jetson 远程桌面终端重新启动应用时执行：

```bash
cd ~/smartlock_demo
cam=/dev/v4l/by-id/usb-Generic_USB2.0_PC_CAMERA-video-index0
./build-stable/smartlock_gui --db data/live-lesson.json --camera "$cam"
```

1. 面向镜头。等预览里只出现一张清晰人脸，点「截帧用于录入」。画面会停在当前帧供你确认。
2. 填入代号，点「录入当前图片」。已有模板时直接启动摄像头。连续 3 帧通过同一代号后，窗口显示「实时授权通过」，会话记录增加一条「模拟开锁」。
3. 在镜头前停留几秒，记录应只增加一次。短暂转脸再恢复，标题应保持授权。即使长时间低分后重新匹配，只要画面里持续检测到人脸，记录也应仍是一条。
4. 完全离开画面约两秒，再回来。状态机在连续一秒没有检测到人脸后恢复开锁资格。重新连续通过 3 帧后，应该增加第二条。未录入的人入镜时，详情行显示「拒绝 · 未知用户」。
5. 点击「停止摄像头」释放设备。截帧留在窗口内供录入，模板库按录入结果保存特征。

### 实时判决怎么验证

```bash
cd ~/smartlock_demo/build-stable
ctest -R live_decision -V
```

测试直接喂入帧序号、采集时间和人脸检测结果。覆盖第三帧触发、换人或跳帧重新计数、999 毫秒仍保留授权、持续有人脸但低分后恢复匹配时去重，以及无人脸满 1000 毫秒后再次触发。

按这三段代码阅读：

1. [`LiveDecision::observe()`](src/live_decision.cpp) 同时接收该帧是否检测到人脸、匹配代号、帧号和采集时间。连续三帧匹配负责界面授权，连续一秒无人脸负责开锁复位。这两个条件分开后，有人脸但分数低时界面可以回到等待，恢复匹配时也不会重复开锁。
2. [`CameraSession::processFrames()`](src/camera_session.cpp) 每完成一次推理就更新判决。输入过快时可以覆盖待处理帧，因此用真实帧号识别断档。Qt 只取最新画面，开锁事件单独交付，界面跳过一次绘制时也能写出已产生的事件。
3. [`DemoWindow::pollCamera()`](src/demo_window.cpp) 用稳定后的判决更新主标题，详情保留本帧相似度，模拟开锁只在收到触发事件时写入会话记录。

也可以从另一条 SSH 终端观察状态切换：

```bash
cd ~/smartlock_demo
tail -f build-stable/live-camera-session.log
```

启动实时模式时先输出 `live decision=waiting`。确认后应看到 `live decision=authorized` 和一条 `live simulated_unlock`。如果从远程桌面终端手动启动应用，这些输出会出现在启动它的终端。

## 6. 测输入 FPS、处理 FPS 和 P95

界面每次启动摄像头都自动预热 2 秒并测量 20 秒，随后显示测量结果。要输出一条便于保存的 JSON，先停止当前窗口的摄像头，再从 Jetson 远程桌面终端运行：

```bash
cd ~/smartlock_demo
cam=/dev/v4l/by-id/usb-Generic_USB2.0_PC_CAMERA-video-index0
./build-stable/smartlock_gui --db data/live-lesson.json --camera "$cam" --measure-seconds 20
```

两项 FPS 分别用成功读帧数、识别结果完成数除以同一段测量时间。识别 P95 从 OpenCV 交付帧计到结果就绪，含排队。Qt 预览 P95 从同一起点计到 Qt 窗口更新预览，计时终点为预览提交给控件。JSON 的 `authorized_frames` 等字段统计模型的逐帧结果，可和稳定后的状态切换对照。基线在 Jetson 的 VNC 桌面上以 640×480、无人脸场景测得：

| 指标 | 基线一 | 基线二 |
|---|---:|---:|
| 输入 FPS | 20.00（400 帧） | 16.65（333 帧） |
| 处理 FPS | 19.95（399 帧） | 16.65（333 帧） |
| 识别 P95 | 35.9 ms | 36.7 ms |
| Qt 预览 P95 | 51.5 ms | 52.6 ms |
| 等待推理时被新帧替换 | 0 帧 | 0 帧 |

想比较有人脸时的开销，可在启动摄像头前先录入模板，然后分别测量空画面和持续单人入镜的画面。

## 7. 查看和删除人脸模板

在 Jetson 远程桌面终端用两个公开样例建一份临时库，方便随时重做：

```bash
cd ~/smartlock_demo
lesson_db="$(mktemp -d)/faces.json"
./build-stable/smartlock --db "$lesson_db" enroll '样例 A' assets/samples/enroll.jpg
./build-stable/smartlock --db "$lesson_db" enroll '样例 B' assets/samples/unknown.jpg
./build-stable/smartlock_gui --db "$lesson_db" assets/samples/known.jpg
```

1. 点「人脸模板…」，列表应显示样例 A、样例 B。选中 A，点「删除所选」。确认框应显示 A，默认选中「取消」。
2. 点「取消」，列表应仍有两条。再删除 A，这次点「删除」，列表应立即只剩 B。
3. 关闭列表，点「验证当前图片」。窗口加载的是 A 的另一张照片，应显示「拒绝 · 未知用户」。选择 `assets/samples/unknown.jpg` 再验证，B 应能通过。删完最后一条再验证时，窗口会提示先录入用户。
4. 要查看已有的实际模板，用同一程序并指定 `--db data/live-lesson.json`。管理列表按当前窗口启动时指定的库读取。

从 [`DemoWindow::manageTemplates()`](src/demo_window.cpp) 找到管理按钮的事件处理，跟踪所选名称怎么进入确认框。点取消会在调用写入路径之前返回。

[`FaceService::removeTemplate()`](src/face_service.cpp) 和录入共用一把文件锁，锁住重新读取、修改和保存。这样即使另一进程同时录入，删除也只移除目标模板。保存通过 `QSaveFile` 原子替换，删完最后一条会留下可继续使用的空库。

摄像头运行时，[`CameraSession::removeTemplate()`](src/camera_session.cpp) 把删除和推理串行执行。提交成功后清除该模板的候选、授权与待显示事件，并保留本次入镜已开锁的状态。

要复跑自动验证：

```bash
cd ~/smartlock_demo/build-stable
ctest -R 'gui_smoke|live_decision' -V --timeout 30
```

Qt 测试点击真实按钮，核对取消前后库文件字节一致、确认后只删所选项、其他模板仍可验证，以及删除最后一条后重读仍为空。实时判决测试还检查删除模板时会清除该模板的旧判决。

## 8. 串口指纹

参考项目通过 UART 接入指纹模块。此处选择 **GROW R503 电容指纹模块的 3.3V UART 版本**。模块负责采集、生成模板、保存和搜索，Jetson 读取操作结果、槽位和匹配分数。 [参考项目](https://gitee.com/huang-haitao1288/intelligent-door-lock-project) · [R503 厂商规格](https://www.hzgrow.com/product/68.html) · [模块协议手册](https://cdn-shop.adafruit.com/product-files/4651/4651_R503%20fingerprint%20module%20user%20manual.pdf)

### 先用伪终端演练

在 Jetson 远程桌面开两个终端。终端 A 启动模拟器并保持运行：

```bash
cd ~/smartlock_demo
python3 tools/fingerprint_simulator.py
```

记下它打印的 `PORT=/dev/pts/…`。终端 B 用实际显示的路径启动 Qt，例如：

```bash
cd ~/smartlock_demo
./build-stable/smartlock_gui --fingerprint-port /dev/pts/7
```

1. 点「指纹…」→「连接检查」。应显示「伪终端演练」和容量 200。
2. 选择空槽位 1，点「录入指纹」。依次按 Qt 的提示，在终端 A 输入 `A` 回车、空行回车、再输入 `A` 回车。它们对应第一次按压、松开、第二次按压。应显示「录入成功 · 槽位 1」。
3. 在 A 输入空行松开，点「验证指纹」，按提示输入 `A`。应显示匹配槽位 1，主窗口新增一条「模拟开锁 | 指纹（演练） · 槽位 1」。
4. 再输入空行松开，点验证并输入 `B`。应显示「拒绝 · 指纹未匹配」，主窗口的模拟开锁记录仍是一条。终端 A 输入 `q` 可结束演练，模拟槽位只在该进程运行期间保存。

### 买到模块后接线

准备 R503 3.3V UART 版、配套线束、独立 USB 转 TTL 串口模块。确认转接板的 **TX/RX 逻辑电平是 3.3V**，并确认供电端能提供稳定的 3.3V。转接板供电能力不明确时，使用满足模块电流要求的独立 3.3V 稳压电源并共地。引脚以手上模块版本的说明书为准。

| R503 | Jetson 侧 |
|---|---|
| 主供电 VCC/VIN | 稳压 3.3V |
| GND | USB 转 TTL 的 GND，独立电源也接这个地 |
| TXD | USB 转 TTL 的 RXD |
| RXD | USB 转 TTL 的 TXD |

先断电核对接线，再把转接板插到 Jetson 的 USB 口。按插拔前后的差异找新增的 `/dev/serial/by-id/` 路径，避免使用已有的底盘串口。Jetson 的当前用户已在 `dialout` 组。默认串口参数为 `57600 8N1`，驱动以模块的出厂地址和密码探测。

```bash
cd ~/smartlock_demo
ls -l /dev/serial/by-id/
port=/dev/serial/by-id/你的指纹转接板
python3 build-stable/fingerprint_uart.py --port "$port" probe
./build-stable/smartlock_gui --fingerprint-port "$port"
```

连接检查成功后，按 Qt 的提示用同一根手指录入两次。验证时分别用已录入手指和另一根手指，检查只有已录入手指匹配并写入一次模拟开锁。录入前会读模块槽位表，占用的槽位会提示改用空槽位。

### 按执行顺序读代码

1. [`FingerprintDialog::startOperation()`](src/fingerprint_dialog.cpp) 用 `QProcess` 启动串口助手，等待手指期间 Qt 仍可处理窗口事件。窗口只在验证正常完成且模块明确返回匹配时发出开锁事件。
2. [`SerialPort.exchange()`](tools/fingerprint_uart.py) 封装 EF01 命令，处理串口分段读取、超时、应答类型与校验和。 [`Sensor.enroll()`](tools/fingerprint_uart.py) 负责两次按压和模块写槽，`verify()` 负责搜索。
3. [`DemoWindow::openFingerprint()`](src/demo_window.cpp) 接收验证事件，将指纹结果和人脸结果写入同一个模拟开锁记录入口。

```bash
cd ~/smartlock_demo/build-stable
ctest -R fingerprint -V --timeout 30
```

自动验收使用实际 Linux 伪终端。串口测试覆盖连接、双次录入、匹配、未匹配、占用槽位、采集不一致、超时和坏校验包。Qt 测试点击实际按钮，确认录入和拒绝不产生开锁记录，成功验证产生一条标明「演练」的记录。实体 R503 通信与指纹识别待接线后实测。

## 9. Grove 手势与 Qt 模拟开锁

已在 Jetson 的 `/dev/i2c-7` 上确认 Grove Gesture v1.0。模块用物理针脚 1（3.3 V）、3（SDA）、5（SCL）、6（GND）。

```bash
cd ~/smartlock_demo
cmake -S . -B build-gesture -DCMAKE_BUILD_TYPE=Release
cmake --build build-gesture -j2
python3 tools/gesture_paj7620.py probe
python3 tools/gesture_paj7620.py watch --seconds 20
```

1. `probe` 应返回 `0x7620`。这是寄存器 0x00、0x01 返回的 `20 76` 按低字节在前合成的 ID。
2. `watch` 显示动作码。让感应面朝向手掌，保持约 5–10 厘米。先分别试左右、上下，再平行于板面画小圆。板子转动后直线方向也会变，可以用读数校准摆放方向。项目使用 Seeed 针对这款 Grove 模块给出的方向码，`0x002` 是左、`0x004` 是上、`0x040` 是顺时针。
3. 等命令行监听结束，打开 Qt。点「手势…」→「开始验证」，按提示完成左、上、顺时针。每轮 20 秒。每笔之后停约半秒，在 5 秒内做下一笔。通过后关掉对话框，主窗口应新增一条「模拟开锁 | 手势 · 左 → 上 → 顺时针」。再试错误顺序或超时，会话记录应保持原来的条数。

```bash
cd ~/smartlock_demo
./build-gesture/smartlock_gui --db data/live-lesson.json
cd build-gesture
ctest -R '^gesture_' --output-on-failure
```

按运行顺序阅读：

1. [`Sensor.transfer()`](tools/gesture_paj7620.py) 一次提交寄存器地址和读操作，保证两个阶段在同一个 I²C 事务里。初始化将连续旋转累计阈值从 32 调到 24。随后，`read_flags()` 从 0x43、0x44 读芯片已识别的手势。
2. 同一文件的 `GestureSequence.accept()` 记住当前等待的动作，使用单调时钟判断间隔。每个正确动作后的半秒用于收手，期间的事件不影响进度。之后遇到错误动作或混合标志会重置进度。每次验证成功就结束这一轮。
3. [`GestureDialog::finish()`](src/gesture_dialog.cpp) 在 Python 助手正常退出并报告验证通过后发出信号。主窗口沿用已有的模拟开锁记录入口。命令行和 Qt 每次只能有一个助手监听，避免两边争读同一个事件。

`gesture_sequence` 检查顺序、混合标志和超时。`gesture_gui` 用测试助手验证 Qt 会话记录，覆盖读取、拒绝、异常退出和通过四种情况。

- [Seeed Grove Gesture v1.0 模块文档及示例](https://wiki.seeedstudio.com/Grove-Gesture_v1.0/)
- [Seeed Grove_Gesture 初始化代码](https://github.com/Seeed-Studio/Grove_Gesture)
- [PixArt PAJ7620U2 数据手册第 7.5 节](https://files.seeedstudio.com/wiki/Grove_Gesture_V_1.0/res/PAJ7620U2_DS_v1.5_05012022_Confidential.pdf#page=38)

## 10. 查询模拟开锁历史

```bash
cd ~/smartlock_demo
cmake -S . -B build-history -DCMAKE_BUILD_TYPE=Release
cmake --build build-history -j2
./build-history/smartlock_gui --db data/live-lesson.json
```

1. 用已录入人脸或手势序列通过一次验证。点击主窗口右侧的「开锁历史…」，查看时间、授权方式和姓名或凭据。列表先显示最新写入的记录。
2. 用上方下拉菜单按方式筛选。未知用户和失败的手势不会进入模拟开锁历史。指纹演练通过时会标明「指纹（演练）」。
3. 关掉 Qt，再用同一条命令启动。再次打开历史应看到刚才的记录。要看运行期间新增的记录，重新打开历史窗口。

历史保存在模板库旁边。上面的命令对应 `data/live-lesson.json.unlock-history.json`，只保留最近 500 条成功的模拟开锁。文件存 UTC 时间，Qt 按 Jetson 的系统时区显示。旧版界面的会话记录只在内存中，升级后从首次成功验证开始写盘。

从 [`DemoWindow::recordSimulatedUnlock()`](src/demo_window.cpp) 开始读。它把各授权方式的成功事件交给 [`UnlockHistory::append()`](src/unlock_history.cpp)。文件锁避免两个窗口并发写入时覆盖对方的记录，`QSaveFile` 保证读者看到完整文件。若磁盘写入失败，会话记录会提示具体错误。

```bash
cd ~/smartlock_demo/build-history
ctest -R 'gui_smoke|gesture_gui|fingerprint_gui' --output-on-failure
```

## 11. 联合验证与模拟锁舌

在 Jetson 的桌面终端进入工程目录，使用独立构建目录启动新版。需要同时接好 USB 摄像头和 Grove 手势模块，并有一个已录入的人脸模板。

```bash
cd ~/smartlock_demo
cmake -S . -B build-combined -DCMAKE_BUILD_TYPE=Release
cmake --build build-combined -j2
(cd build-combined && ctest -R 'face_gesture_challenge|gesture_gui|live_decision' --output-on-failure)
./build-combined/smartlock_gui --db data/live-lesson.json
```

1. 勾选「实时人脸＋手势联合验证」，启动摄像头。已录入的人脸通过连续帧判决后，界面提示「人脸已确认」，会话此时还没有新的开锁记录。
2. 点「手势… → 开始验证」，在 30 秒联合验证窗口内完成「左 → 上 → 顺时针」。窗口从摄像头采到触发授权的那帧开始计时。成功时，开锁历史新增一条「人脸＋手势」，模拟锁舌显示已解锁，3 秒后回锁。
3. 用同一个模板重试时，先离开镜头等实时判决复位，再入镜。验证时也可以测试等待超过 30 秒，或关闭手势窗口再打开。失效的验证资格无法写入开锁历史。图片验证可继续查看匹配结果，指纹入口在联合验证模式下停用。
4. 取消勾选可恢复单项验证。所有通过的模拟开锁都会驱动同一个界面锁舌状态。

`FaceGestureChallenge` 用单调时钟保存到期时间，并给每轮资格编号。Qt 打开手势窗口时记住本轮编号，完成后只能消费同一份资格一次。摄像头停止、模式切换、删除当前候选模板或关闭本轮手势窗口时会取消资格。模拟回锁使用 `QTimer`，等待期间窗口仍能处理图像和输入。

## 12. 在 Jetson 上用硬件 PWM 驱动模型锁舌

默认启动仍使用界面模拟锁舌。添加 `--servo-pwm` 后，实时人脸、手势、实体指纹或联合验证的成功事件会请求硬件 PWM 开锁。图片人脸和指纹演练保留模拟开锁行为。Qt 显示「指令已发送」，舵臂是否实际到位需要现场观察。

1. 核对载板的排针定义，并把物理 Pin 33 的 pinmux 切到对应的 PWM 输出。NVIDIA Jetson.GPIO 的 Orin NX 参考映射是 `32c0000.pwm`、通道 `0`。先确认你的载板也使用这套映射。在 Jetson 上查控制器路径，每个 `pwmchipN` 后面显示它的实际硬件设备。

   ```bash
   for chip in /sys/class/pwm/pwmchip*; do
     [ -d "$chip" ] && printf '%s -> %s\n' "$chip" "$(readlink -f "$chip/device")"
   done
   ```

2. 断电时把 SG90 橙线接到确认过的 Pin 33，舵机电源按型号规格独立供电，棕线与 Jetson 共地。第一次空载测试，让白色舵臂有足够的转动空间。
3. 用有 PWM sysfs 写权限的桌面账号运行。账号需要能写控制器的 `export`、`unexport` 和导出通道的 `period`、`duty_cycle`、`enable`。Jetson 可参照 NVIDIA Jetson.GPIO 的 `gpio` 用户组与 udev 规则。结束此前占用通道的测试进程，由本程序导出和释放空闲通道。

   ```bash
   cd ~/smartlock_demo
   cmake -S . -B build-pwm -DCMAKE_BUILD_TYPE=Release
   cmake --build build-pwm -j2
   (cd build-pwm && ctest -R servo_pwm --output-on-failure)

   # 把 N 换成第一步核对到的编号。
   ./build-pwm/smartlock_gui --db data/live-lesson.json \
     --servo-pwm /sys/class/pwm/pwmchipN --servo-channel 0
   ```

4. 启动时输出 `20 ms` 周期、`1.5 ms` 高电平的回锁信号。用已录入人脸或手势授权一次，预期舵臂随 `2.0 ms` 脉宽偏转，3 秒后切回 `1.5 ms` 回位。回锁写入失败时界面会显示故障，每隔 1 秒重试，恢复前暂停新的 PWM 开锁。正常退出时程序尝试回位，等待约 350 ms，再禁用和释放 PWM 通道。进程被强杀或断电时，软件回锁定时器无法执行。

`PwmServo` 负责 sysfs 读写和通道生命周期。`DemoWindow::requestUnlock()` 在授权后调用它，`relock()` 在定时器到期时切回回锁脉宽。本地 `servo_pwm` 测试使用临时目录模拟 sysfs，真正的 Pin 33 波形和舵臂行程需要上板核验。

- [NVIDIA Orin NX 排针映射源码](https://github.com/NVIDIA/jetson-gpio/blob/master/lib/python/Jetson/GPIO/gpio_pin_data.py)
- [NVIDIA Jetson.GPIO 权限与 PWM 配置](https://github.com/NVIDIA/jetson-gpio#setting-user-permissions)

## 13. Pin 33 重启恢复与设备自检

启动服务仅用于已经完成实机验证的 P3768 载板和 P3767-0000 模组组合。它在每次开机时检查 Pin 33 的寄存器，只处理已知的默认值 `0x54` 和目标值 `0x405`。回读成功后写入本次启动的状态文件。服务只配置引脚复用，由 Qt 决定何时启用 PWM 和发送开锁指令。

```bash
sudo install -m 0755 tools/smartlock-pin33-pwm5 /usr/local/sbin/smartlock-pin33-pwm5
sudo install -m 0644 tools/smartlock-pin33-pwm5.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now smartlock-pin33-pwm5.service
systemctl status smartlock-pin33-pwm5.service --no-pager
cat /run/jetson-smartlock/pin33-pwm5
```

升级源码后按第 12 节重新构建。启动参数 `--servo-pin33` 会检查本次启动的复用记录，并按 `32c0000.pwm` 查找 Linux 当前分配的 `pwmchip` 编号。

```bash
./build-pwm/smartlock_gui --db data/live-lesson.json --servo-pin33
```

点主窗口的「设备自检」查看 V4L2 采集能力、PAJ7620U2 芯片 ID、PWM 通道读数和 Pin 33 的启动记录。手势正在验证时，自检可能报告 I²C 已在使用，等本轮结束后重新检查。自检只查询状态，硬件是否实际回位可用空载手势测试确认。停用启动服务可运行 `sudo systemctl disable --now smartlock-pin33-pwm5.service`，当次寄存器值在下次重启后恢复系统启动配置。

## 14. 锁舌到位反馈

使用一只带 `COM`、`NO` 的无源机械微动开关和一只 10 kΩ 电阻。把开关固定在锁舌伸出后的回锁位置，确保锁舌到位时才压下开关。接线前关闭 Jetson 和舵机电源。

| 连接 | 这台 Jetson 的物理针脚 |
| --- | --- |
| 开关 COM | Pin 39（GND） |
| 开关 NO | Pin 7（GPIO） |
| 10 kΩ 电阻两端 | Pin 7 和 Pin 17（3.3 V） |

上拉电阻让释放时的 GPIO 保持高电平，开关闭合时 GPIO 被拉低。5 V 舵机电源只用于舵机，反馈输入使用 Jetson 的 3.3 V。当前板子的 Pin 7 对应 `/dev/gpiochip0` 的 line offset 144（`PAC.06`）。换载板或系统后先核对 GPIO 映射。

开关和电阻装好后，在新版工程目录启动 Qt，继续使用原有模板库。

```bash
./build-pwm/smartlock_gui --db "$HOME/smartlock_boot_IwMwqe/demo-faces.json" \
  --servo-pin33 --latch-gpio /dev/gpiochip0:144
```

1. 打开「设备自检…」。锁舌压下开关并稳定约 60 ms 后，自检显示「锁舌已到位」。
2. 做一次有效手势。锁舌应退出回锁位置并释放开关，约 3 秒后回锁，重新压下开关。会话里先记录 PWM 回锁指令，再记录传感器确认。
3. 用空载台架让开关保持释放来测试超时。回锁指令发出后约 1.5 秒，Qt 应显示「回锁超时 · 未确认锁舌到位」。如果开关全程始终闭合，Qt 会报告「限位触点未释放」。

未安装开关时省略 `--latch-gpio`。超时或读错只更新反馈和会话记录，程序继续保持已下发的回锁脉宽。单个触点只能确认回锁限位，释放意味着离开该限位。

## 模型和样例来源

项目根据参考仓库的处理流程重新实现，使用 OpenCV 4.5.4 的 SSD 配置和模型，以及 OpenFace 的 nn4.small2.v1 权重。OpenFace 权重经 KDE 北外镜像下载，校验值与上游发布文件一致。三张图是公开的 face_recognition 示例图，前两张是同一人不同照片。素材由 `assets/SHA256SUMS` 校验。

- [智能门锁参考项目](https://gitee.com/huang-haitao1288/intelligent-door-lock-project)
- [OpenCV 4.5.4 模型清单](https://raw.githubusercontent.com/opencv/opencv/4.5.4/samples/dnn/models.yml)
- [OpenFace 官方模型说明](https://github.com/cmusatyalab/openface/blob/master/docs/models-and-accuracies.md)
- [公开测试图片](https://github.com/ageitgey/face_recognition/tree/master/examples)
