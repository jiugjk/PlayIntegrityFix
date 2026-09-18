# Play Integrity Fix

**简体中文** | [English](README.en.md)

本模块用于修正 Play Integrity 的检测结果，以获得有效的设备认证（attestation）。

## 注意

本模块**不是**用来隐藏 root 的，也不用于绕过其他应用的检测。它只负责让设备通过 Play Integrity 测试中的 Device 认证等级。

## 使用前提

你需要 root 和 Zygisk：启用 Magisk 内置的 Zygisk，或安装 [ZygiskNext](https://github.com/Dr-TSNG/ZygiskNext) / [NeoZygisk](https://github.com/JingMatrix/NeoZygisk) / [ReZygisk](https://github.com/PerformanC/ReZygisk)。

## 选项

- **spoofBuild**：伪装 fingerprint 字段，默认开启。
- **spoofProvider**：自定义 keystore provider，未使用 [TrickyStore](https://github.com/5ec1cff/TrickyStore) 或 [TEESimulator](https://github.com/JingMatrix/TEESimulator) 时开启。
- **spoofProps**：当 GMS 从 system prop 读取时伪装属性，未使用 [TrickyStore](https://github.com/5ec1cff/TrickyStore) 或 [TEESimulator](https://github.com/JingMatrix/TEESimulator) 时开启。
- **spoofSignature**：伪装 ROM 签名，当你的 ROM 由 testkey 签名时开启。可在终端执行以下命令查看 ROM 签名：
  ```sh
  unzip -l /system/etc/security/otacerts.zip | grep -oE "testkey|releasekey"
  ```
- **spoofVendingSdk**：若设备运行 Android 13 及以上，向 Play Store 伪装 SDK 版本为 32；设备为 Android 12 及以下时该选项不生效。
  - 已知问题：
    - 在 Play Store 内使用返回手势/返回键会直接退到桌面（所有设备）
    - Android 14+ 的 ROM 上账号登录状态为空、应用更新异常
    - 可能被分发到错误的应用变体（所有设备）
    - 某些配置下 Play Store 会完全崩溃
  - pixel_beta fingerprint 即便走旧版校验也已无法取得 Device 认证，因此开启此选项不再有助于拿到 Device verdict。

## 分支版本

- **inject-vending**：基于官方 inject 分支，增加了 spoofVendingSdk 选项。
- **inject-manual**：基于 inject-vending，移除了自动配置（检测 TrickyStore 与 ROM 签名）。
- **inject-s**：基于 inject-manual，放弃 JSON 格式（pif.json → pif.prop），更轻量。

> [!NOTE]
> **inject-vending** 与 **inject-manual** 分支已停止维护，但分支本身不会被删除。

> [!WARNING]
> 请勿使用第三方工具抓取 fingerprint，以免产生冲突。
> WebUI 已提供全部可配置项的完整控制。

### 关于 chiteroman 的官方 PIF

- [chiteroman 的官方 PIF](https://github.com/chiteroman/PlayIntegrityFix) 已从 GitHub 下架。
  - 官方原始 main 分支：https://github.com/KOWX712/PlayIntegrityFix/tree/main
  - 官方原始 inject 分支：https://github.com/KOWX712/PlayIntegrityFix/tree/inject
  - 官方 PIF 仓库的所有 tag 也都保留在本仓库中。

## 致谢

- [kdrag0n](https://github.com/kdrag0n/safetynet-fix) 与 [Displax](https://github.com/Displax/safetynet-fix) 提供了最初的思路。
- 本项目 fork 自 chiteroman 的官方 PIF 仓库。
- [osm0sis](https://github.com/osm0sis) 编写了最初的 [autopif2.sh](https://github.com/osm0sis/PlayIntegrityFork/blob/main/module/autopif2.sh) 脚本，[backslashxx](https://github.com/backslashxx) 与 [KOWX712](https://github.com/KOWX712) 对其进行了改进（[action.sh](https://github.com/chiteroman/PlayIntegrityFix/blob/main/module/action.sh)）。
