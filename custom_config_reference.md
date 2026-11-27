# 智伴 1X (`xiaozhi-zhiban`) 自定义配置速查手册

> 所有配置均存放在设备 `/var/upgrade/`（FAT32 可读写分区）下，修改后除注明“热生效”外，通常需执行 `adb shell "killall sair"` 让守护进程拉起 `sair` 生效。

---

## 一、本次新增的自定义配置

| 文件路径 | 作用 | 默认值 / 无文件时行为 | 格式与示例 | 生效方式 |
| :--- | :--- | :--- | :--- | :--- |
| `/var/upgrade/.wake_word` | 覆盖主唤醒词（拦截 `libsair_asr.so` 的 `get_config("MAIN_WAKE_WORD")`） | 使用原厂 `config.bin`（`zhi ban zhi ban`） | 单行空格分隔拼音，如：<br>`dan zai dan zai` | 重启 `sair` |
| `/var/upgrade/.wake_thresh` | 覆盖主唤醒词灵敏度阈值（仅配合 `.wake_word` 生效；**越小越灵敏**） | `0.20`（当 `.wake_word` 存在且无此文件时） | 单行浮点数（建议 `0.12` ~ `0.28`），如：<br>`0.15` | 重启 `sair` |
| `/var/upgrade/.speak_gate` | `Speaking`（播报/讲故事）状态下的近场噪声门限，过滤远距离旁人说话误打断 | `600`（`Listening` 状态自动归 `0`） | 单行非负整数：<br>`0` = 关闭门限<br>`600` = 默认<br>`800`~`1000` = 更抗干扰 | **热生效**（每次进入 `Speaking` 时读取） |
| `/var/upgrade/wakeup.mp3`<br>`/var/upgrade/wakeup.wav` | 自定义本地唤醒提示音（优先 `.mp3`，其次 `.wav`） | 播放内置 180ms 柔和双音“叮咚”提示音（`880Hz → 1175Hz`） | 标准 MP3 / WAV 音频文件 | **热生效**（每次唤醒时检查播放） |
| `/var/upgrade/reminders.json` | 本地定时提醒持久化数据（最多 32 条，支持一次性与每日重复） | 空列表 `[]` | JSON 数组（由 `self.reminder.*` 自动读写） | 自动持久化 |

### 常用快捷命令

```bash
# 1. 设置自定义唤醒词与阈值（推荐使用 set_wake_word.py 自动处理多音字与阈值）
adb shell "echo 'dan zai dan zai' > /var/upgrade/.wake_word && echo '0.15' > /var/upgrade/.wake_thresh && killall sair"

# 2. 恢复原厂唤醒词（智伴智伴）
adb shell "rm -f /var/upgrade/.wake_word /var/upgrade/.wake_thresh && killall sair"

# 3. 动态调整播报期防打断门限（无需重启）
adb shell "echo 800 > /var/upgrade/.speak_gate"   # 提高门限（更不容易被旁人打断）
adb shell "echo 400 > /var/upgrade/.speak_gate"   # 降低门限（近距离打断更灵敏）
adb shell "echo 0 > /var/upgrade/.speak_gate"     # 临时关闭门限

# 4. 推送自定义唤醒提示音（无需重启）
adb push my_wakeup.mp3 /var/upgrade/wakeup.mp3
# 恢复内置“叮咚”音
adb shell "rm -f /var/upgrade/wakeup.mp3 /var/upgrade/wakeup.wav"
```

---

## 二、固件原有 `/var/upgrade/` 配置备忘

| 文件路径 | 作用 | 说明 |
| :--- | :--- | :--- |
| `/var/upgrade/.listening_mode` | 对话监听模式 | 已默认改为 `realtime`（连续对话），也可写入 `auto` |
| `/var/upgrade/.aec_mode` | 回声消除模式 | 与原生 `FFVP` 上行通道配合使用 |
| `/var/upgrade/.ws_config` | WebSocket 服务端地址覆盖 | 自定义云端 WS 地址 |
| `/var/upgrade/.client_id` | 自定义设备 `client_id` | 用于标识客户端会话 |
| `/var/upgrade/.mcp_endpoint` | 自定义 MCP 端点 | 扩展外部 MCP 服务 |
| `/var/upgrade/.boot_push_disable` | 抑制原厂开机推送广告 | 写入 `1` 启用抑制 |
| `/var/upgrade/.rec_dump` | 录音调试转存开关 | `touch /var/upgrade/.rec_dump` 启用会话期间录音 dump |
| `/var/upgrade/.reload_config` | 触发配置重载信号文件 | 创建后主循环自动读取并删除 |
| `/var/upgrade/use_limit_exhausted.mp3` | 使用时长耗尽提示音 | 达限后播放的自定义 MP3 |

---

## 三、调试常用日志过滤

```bash
# 查看唤醒词拦截、唤醒触发、状态流转与防打断门控
adb shell "tail -f /var/upgrade/xiaozhi.log | grep -E 'WAKEUP|ASR|STATE|PROTO|REMINDER|MUSIC|REC'"
```
