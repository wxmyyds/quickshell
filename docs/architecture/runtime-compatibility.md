# 运行时边界

Shell 的原生 module 与 QML 源码由同一次 CMake 构建产生，所有自制 QML import 使用
无版本 URI。产品版本和 JSON `schemaVersion` 独立于 QML import 版本。

系统监测协议由 `key sysmon stream --format jsonl` 提供，Shell 校验
`schemaVersion`、时间戳、序列号和模块字段；失联时显示明确的 stale/error 状态。
录屏、录音和剪贴板协议由 `key-cli` 提供，Shell 只消费参数数组和机器 JSON。

天气、Cava、歌词和 Niri 状态在 Shell 进程内提供响应式模型，避免高频数据经过 CLI
或 Python 中转。

`Clavis.Lyrics` 由 MPRIS 选择当前播放器。Cirrus 只通过 Linux observation v1 Unix
socket 提供歌词；只读取 `$XDG_RUNTIME_DIR/com.guang233.Cirrus/i-*/observation-v1.json`
发现端点，检查所有者、私有权限及符号链接，并用 `SO_PEERCRED` 的 UID/PID 对照当前
MPRIS unique owner 的异步 D-Bus PID 查询。manifest 的 PID 只是提示，不能认证端点。
共享默认关闭、连接断开或播放器退出时立即清空；目录监视及有上限间隔的重试负责发现
重新开启的导出。其他播放器仅展示 `xesam:asText`，不获取网络歌词或读取歌词文件、缓存。

一次订阅接收 snapshot/lyrics/cursor，正文按 trackId、viewEpoch、lyricsRevision
分页读取，每页最多 128 行；最多 4 个未完成请求，发送使用每秒 16 帧、突发 16 帧的
令牌桶。六条原始轨道及真实逐字时间全部保留在 native document 中。展示内容优先选
`original.enhanced`（availability 为 present），否则选 `original.standard`；这是内容
偏好，不在错误时替换轨道。翻译、罗马音采用相同内容偏好，按含各自 provider offset
的时间戳合并，独立行号不直接对应。

本地时间轴以快照 songPositionMs 为准，已经包含试听起点；比较歌词时各减一次
providerOffsetMs 和 userOffsetMs。只有 playing 状态向前推算，收到快照后最多推算
1 秒，使用单次边界定时器更新行和逐字游标，不重建正文。纯文本显示第一条非空行，
首个时间戳之前留白，间隙保留最后开始的一行。QML 仅展示，不提供滚动、拖动、点击或 seek。
