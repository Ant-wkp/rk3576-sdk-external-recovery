# 板端启动检查故障夹具

这些文件仅用于已备份的 DShanPi A1 eMMC A/B 测试板。它们不在生产
`Makefile`、Buildroot 安装规则或正式镜像中，不应进入发布 rootfs。
先运行上级目录的隔离测试，再进行板端验收。串口保持单一接收器；归档走网口。

## 构建

使用 SDK Buildroot 实际交叉编译器及其配套 sysroot，例如在 SDK Docker 中：

```bash
TEST_CC=/workspace/rk3576_SDK/buildroot/output/rockchip_rk3576_ab_test/host/bin/aarch64-buildroot-linux-gnu-gcc
"$TEST_CC" -shared -fPIC -O2 -Wall -Wextra -Werror board_fault.c -ldl -o board_fault.so
"$TEST_CC" -O2 -Wall -Wextra -Werror last_trial.c -o last_trial
```

不要改动全局 `/etc/ld.so.preload`。库只通过单个测试进程的 `LD_PRELOAD` 使用。

## 前置条件和单次故障

两个槽均已安装本轮配套 U-Boot、boot、rootfs；当前槽已经确认；另一槽
镜像健康且未挂载。先保存完整 `misc`、元数据所在512字节扇区、启动镜像
和 userdata 哨兵。确认 by-name 的 `misc` 实际为 `/dev/mmcblk0p2`。

通过网口将库、`oneshot-bootcontrol.sh`、`stage-fault.sh` 放到
`/userdata/native-boot-check-fixtures/`。逐文件核对 SHA-256。

以当前 B、测试 A 为例：

```bash
sh /userdata/native-boot-check-fixtures/stage-fault.sh a identity a-identity
updateEngine --misc=other
sync
reboot
```

逐条核对返回值及元数据，不把以上示例当成无条件连续执行脚本。
`stage-fault.sh` 挂载目标根、备份/比较正式 `S99_bootcontrol`，放入一次性
启动脚本。一次性脚本在启动检查前原子恢复正式脚本并同步，然后只对实际
`updateEngine --boot-check` 应用夹具。因此再次启动自动走正常检查，不重复故障。
如果准备中断，先核查 pending、目标脚本和元数据，不能盲目重新激活或重放。

| 模式 | 只在检查进程中注入的故障 | 应验证的结果 |
|---|---|---|
| `identity` | `uname()` 返回错误的内核发布身份 | 未确认槽禁用，立即正常重启到已确认旧槽 |
| `hang` | 第一次真实 KEEPALIVE 后 SIGSTOP 自身 | 实际进程为 T，真实 WDT 自动复位，次数只扣一次 |
| `reboot-fails` | 身份错误且 `/sbin/reboot` 的 exec 返回失败 | 先持久化回退，再由真实 WDT 自动复位 |
| `metadata-crc` | 只破坏从已知 misc 读取到内存的 CRC | 不改磁盘元数据，保留 WDT 兜底，不假报回退成功 |

也可以对**当前已确认槽**放置单次 `identity`，不执行 `--misc=other`：正常
重启后检查失败应再次重启同槽，不自动降级。此用法会临时改当前根的启动
脚本，只能在明确的专项验收中执行。

库记录单调时间事件；设置 `UE_BOARD_TEST_TAG` 时保存接管和正常重启前的
512字节只读快照。正常重启前后不能只比较下一次 SPL 已改过的 `last_boot`；
应比较这两个快照，确认旧槽字段、`last_boot`、保留区及480字节尾部保持。
原始串口日志和主机时间戳共同证明真实复位，不能用人工重启代替。

## 最后一次试启动

在正常 `updateEngine --misc=other` 已激活另一健康槽后运行：

```bash
/userdata/native-boot-check-fixtures/last_trial a
sync
reboot
```

该工具只接受与当前命令行相反的槽；要求当前槽14/0/已确认、目标槽15/7/
未确认、元数据头及 CRC 正确。只把目标 tries 从7改为1，完整扇区同步和
回读；不能用于任意元数据编辑。必须观察 SPL1→0 后**同槽完整启动**，
检查通过后 successful=1、tries=0，WDT停止。

## 收尾

归档测试日志、二进制快照及校验值；确认两个根均恢复正式启动脚本，没有
全局 preload、pending 或测试环境变量。测试目录可保留为非自动运行证据，
正式服务不得引用它。恢复健康确认槽，正常关机，再关闭继电器。
