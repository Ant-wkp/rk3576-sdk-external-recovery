# 原生 updateEngine 启动检查：最终验收报告

日期：2026-10-27。设备：100ask DShanPi A1 / RK3576 / eMMC。

**结论：本轮计划已完成。功能已安装在两槽实际 rootfs 中，不是仅运行临时测试程序。**
原生 `updateEngine` 负责有限期启动检查、自动确认、立即回退及成功后停止 WDT。
实板双向完整 OTA、明确失败立即回退、真实 WDT 兜底、末次确认均通过。
最终 B/V2 为健康确认槽，A/V1 保留健康确认状态；开发板已正常关机，继电器 off，
串口及临时传输服务已关闭。

## 1. 实现与修改边界

| 组件 | 本轮交付 |
|---|---|
| Recovery | 原生 `updateEngine --boot-check`、互斥锁、严格槽位解析、可靠元数据读写/确认/禁用失败槽、原 init 入口、隔离与板端测试 |
| Buildroot | 本 A/B defconfig 安装 updateEngine、无 UI、SUCCESSFUL_BOOT、BOOTCONTROL；最终 rootfs 安装及源码/二进制缓存门禁 |
| device/rockchip | 两套 overlay 发布身份生成、清理旧喂狗启动项 |
| BSP kernel | 两套 NOWAYOUT 关闭、单 APB 复位、DW WDT 停止返回值及硬件状态验证 |

SPL/U-Boot 源码未修改。本轮重新构建并部署与发布文件完整版本相匹配的现有
U-Boot proper FIT；共享 SPL/Loader、GPT、TF-A、OP-TEE 未更新。
未改旧独立 OTA 工程、主线、NFS、TFTP、OTP/eFuse、安全启动或上锁配置。
只使用 Loader 定向写入初始化 B 槽，没有全盘升级/擦除/Maskrom。

启动后的固定策略：

```text
S99_bootcontrol 后台启动原生 updateEngine --boot-check
    ├─ 每5秒检查，连续健康30秒，总期限120秒
    ├─ 成功：确认当前槽 → fsync/扇区回读 → 停止WDT → 退出
    ├─ 未确认新槽明确失败：验证旧槽已确认 → 仅禁用新槽 → 回读 → 正常reboot
    ├─ 已确认槽失败：正常reboot，不降级
    └─ 检查卡死/重启失败/不可信元数据：WDT兜底，不盲写、不假报回退成功
```

没有安装独立检查器、长期喂狗服务或业务探针。`--misc=now` 仅保留手动管理用途。
成功关闭 WDT 后，不提供运行期死机监控，这是本轮明确采用的策略。

## 2. 编译、镜像与安装

SDK 在 `ubuntu-dev`（`ubuntu-rk3566:latest`）内完成两套完整 `./build.sh`
及 `./build.sh ota-updateimg`；Recovery 源码变化后执行
`./build.sh bmake:recovery-rebuild`，没有以旧缓存替代新实现。

有效固件仅为证据目录的 `v1-current/` 与 `v2-current/`。旧 `v1/`、
`v2-final/` 明确不可部署。两套 rootfs 均使用同一原生程序：

```text
updateEngine SHA256
788dd953ca0b9f8473778c3c47262e210ffc191dd3311307243df190d2e1d5de
S99_bootcontrol SHA256
530fd9bdec2545d9126541babe076fe6f24b71ec908cbe4595a0d5dfce42025e
```

| 发布 | 内核 | U-Boot 完整 banner |
|---|---|---|
| V1 | `6.1.75-abtest-v1` | `U-Boot 2017.09 (Oct 27 2026 - 02:49:43 +0800)-abtest-v1` |
| V2 | `6.1.75-abtest-v2` | `U-Boot 2017.09 (Oct 27 2026 - 02:40:52 +0800)-abtest-v2` |

共同布局为 `rk3576-ab-v2`，SPL ABI 为 `abctx2`。两槽 `/etc/rk-ota-release`、
实际内核、DTB chosen 身份、U-Boot banner 与实际 root 设备相符。
最终只读挂载另一根分区审计，两槽程序与正式脚本摘要均匹配；没有旧喂狗
启动项、全局 preload、pending，`/usr/bin` 未安装 Recovery/独立 OTA/独立 feeder。
程序实际执行网络下载、镜像解析和刷写，动态库运行依赖得到实板验证。

## 3. 实板验收矩阵

| 项目 | 具体证据与结果 |
|---|---|
| 初始化与备份 | 冷启动唯一 UART；Linux/Loader 分区图一致；网口回传完整 misc、512字节元数据扇区、两套 U-Boot/boot、共享前8MiB，备份摘要全部通过 |
| 初始 B 部署 | upgrade_tool2.44，Loader ID311，USB2207:350e，序列号fee861189bdf4357；只写uboot_b/boot_b/system_b；先回 A，用 Linux 对 B 三载荷实际读回 SHA256 |
| B→A 网络完整 OTA | 已安装 updateEngine 下载 V1并刷非活动 A，返回0，原生校验和额外 SHA均通过；正常重启，A自动确认、WDT停止 |
| A→B 本地完整 OTA | 已安装 updateEngine 从本地 V2包刷非活动 B，返回0，实际读回三项 SHA一致；正常重启，B自动确认、WDT停止 |
| 明确失败立即回退 | A未确认槽只注入检查进程 uname身份错误；检查持久化禁用 A，约3.46秒完成正常重启，下一次选已确认 B，而非等待 WDT |
| 回退持久化与保留区 | 接管/执行reboot前真实512字节快照：只变失败槽字段及CRC；旧槽四字节、last_boot、保留区和480字节尾部保持 |
| 检查卡死 | 实际检查进程 SIGSTOP、状态T、真实WDT active；无人工重启/断电，约89.55秒自动复位；下一次SPL A6→5，正常检查恢复 |
| 正常重启失败 | 检查已持久化禁用 B，再令reboot exec返回EACCES；WDT约89.60秒实际复位，下一次选健康确认 A |
| 元数据错误 | 只在检查进程内破坏实际读取的CRC，磁盘扇区不变；记录错误、不盲写，WDT约89.50秒复位；恢复正式检查后B确认、WDT停止 |
| 最后1次试启动 | 激活B后受限夹具只将7改1；SPL B1→0，仍加载B U-Boot/DTB/kernel/root；自动确认为B15/0/成功，不混槽 |
| 已确认槽失败 | B已确认身份检查失败，约3.41秒正常重启；仍选择B，不禁用/降级，随后正常检查通过 |
| 互斥及重复调用 | 卡死检查持锁时重复请求返回2；本次已完成的检查重复调用返回0、WDT保持inactive；混合boot-check/misc返回2且元数据不变 |
| 真正停止WDT | 新B首次检查通过后到uptime295.49秒仍为同bootID、inactive、无用户态feeder，超过两个实际硬件周期，不复位 |
| 共享区和数据 | 每份检查点的共享前8MiB与userdata哨兵摘要一致；根分区只在尚未挂载时按镜像有效长度比较，不拿运行后ext4变化误判烧录 |
| 正式入口恢复 | 两槽启动脚本与发布原件摘要一致；故障库仅测试进程使用；测试资产可恢复移存非启动归档目录 |
| 收尾 | 健康B确认、A确认保留；正常poweroff看到Power down，继电器feedback-off/status均off；UART释放、8877无监听 |

WDT 时钟为24MHz、TOP15，SPL 报告89479ms，Linux取整报告89秒。
三次兜底复位的 BL31 reset status 均为 `0x1050`；正常软件重启为 `0x1`。
时间测量取一次性启动日志到DDR重新输出，包含少量夹具启动/复位延迟，
不是声称硬件精确89秒，也没有用人工动作代替自动复位。

## 4. 隔离测试及实际产物门禁

- `run.py`：真实源码元数据双向/末次确认/重复确认、CRC及头校验、短写/fsync/
  回读错误、扇区尾部保持；状态机超时/无可信旧槽/确认不确定/停狗错误；
  只读根、未知根及异盘misc；15组实际main参数测试；默认掩码及双向载荷调度。
- `test_integration.py`：实际BSP停止函数6组错误/硬件状态用例；发布身份正确与
  混槽拒绝；缺文件/旧脚本/源码缓存/二进制过期拒绝；FIT空模板与实际签名值区分。
- `check_package.py`：原生解析/MD5和独立SHA绑定实际官方OTA载荷；仅三组件，
  GPT/misc不进入正常OTA写入。
- `check_fit.py`：两份FIT一致、范围检查、没有实际签名值，TF-A/OP-TEE载荷不变。
- 新检查模块及板端夹具严格ARM64编译通过。夹具未链接生产程序，未进入正式rootfs。

隔离测试的错误注入不等于真实断电实验。上述板端测试只使用本轮受控临时
夹具注入所列故障；未知根/异盘misc、真实设备写入返回错误等由隔离测试证明，
没有在板上破坏GPT或真实元数据来扩大实验。

## 5. 证据、提交与使用说明

本机证据根目录：
`/home/ant/Documents/Codex/2026-10-01/uboot/outputs/native-boot-check-20261027`。

- `board-evidence/*.tgz`：板端网口回传；`baseline.tgz`为原始备份，
  各before/during/after命名绑定测试阶段，`final-health.tgz`包含最终安装审计。
- `board-evidence/uart-complete.log`：唯一接收器完整日志；`uart-events.log`为
  主机只读观察器时间戳。事件中可能包含命令回显，结论采用实际输出而非回显。
- `sdk-v1-current.log`、`sdk-v2-current.log`、`host-tests-resumed.log`、
  `integration-tests-resumed.log`、两套SHA256SUMS及package/fit-check JSON。
- `issues.md`：问题与解决记录；`commits.md`：完整本地跨组件提交清单；
  `physical-journal.md`：过程记录，旧阶段报告保留，不覆盖历史验收结论。
- `verify_evidence.py` / `evidence-verification.json`：对22份网口归档、真实
  元数据快照、保留区、哨兵和复位时序的可重复只读断言审计，结果PASS。
  源镜像名称与实际目标路径、parameter打印后跳过的区别见问题记录第14节。

代码/测试交付节点：Recovery `c167716fa`，Buildroot `28aa50988`，
device/rockchip `6efa46b4d`，BSP kernel `80aca98f9`。文档提交另列于清单。
提交均在Repo组件仓库，英文Conventional Commits，未自动push。

实际升级时保留显式保存路径，例如：

```bash
updateEngine --update --image_url=http://server/update-ota.img --savepath=/userdata/update.img
# 检查返回值与升级日志成功后，再正常重启：
reboot
```

不要把原生 `--reboot` 加进以上命令：它仍继承原SysRq立即重启分支，本轮不改
原下载/刷写算法。新的启动检查失败分支才使用正常 `/sbin/reboot`。

## 6. 明确不作出的保证

本轮不认证原生差分事务、原件缓存、任意断电安全、恶意镜像安全、数字签名、
反回滚、MTD全链路或业务健康。原生包解析/差分/下载的继承问题未在此修复。
元数据fsync及回读不是单扇区断电原子性保证。发布身份检查是组件一致性检查，
不是信任链认证。停狗后无长期运行期监控；业务探针和常驻喂狗均未引入。
