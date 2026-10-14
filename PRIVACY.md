# Privacy Policy

**Last updated: August 10, 2026**

Agora is a BYOK Android client. It does not operate a relay for chat completions: model requests go from your device to the provider or endpoint you configure.

## Local data

Conversations, message trees, tasks, loops, memories, prompts, attachments, tool media, settings, and imported models are stored in app-managed databases/files on the device. Secret settings normally use an AES-256-GCM envelope backed by the Android Keystore. Legacy values are accepted as plaintext, and encryption failure deliberately falls back to plaintext rather than losing the value, so device storage protection remains important.

Clearing app data or uninstalling removes app-managed data unless Android backup or a user-created export preserves it.

## Network destinations

Data leaves the device only through features you use:

- messages and attachments go to the selected AI provider;
- title, transcription, image-generation, and embedding requests go to their selected providers;
- search queries go to the selected web-search service;
- MCP calls go to enabled MCP servers;
- Conch and SSH operations go to configured remote devices;
- release metadata can be checked when the app starts, at most once per day;
- the optional rating form sends only the rating, name, email, and comment you explicitly submit to `https://newoether.com/api/rating`;
- after a crash, one pending report is stored locally and the next launch asks whether to send it to `https://newoether.com/crash`. It contains the stack trace, app/Android version, device manufacturer/model, timestamp, and bounded diagnostic event tags, but no conversation text, credentials, or device identifiers.

Crash reports are never submitted automatically. Agora does not include a general analytics path. Third-party endpoints have their own privacy and retention policies.

## Backups and exports

A `.agora` export is a ZIP archive. If you explicitly include API keys or other secrets, those values are unencrypted inside the archive. Protect and delete exported copies as appropriate.

## Transport and proxy

The configured proxy applies to Agora's shared HTTP-client traffic, not direct SSH, local inference, or processes inside the Alpine sandbox. Conch application-layer encryption requires an API key; a blank-key endpoint uses plain JSON and relies on HTTPS for transport confidentiality.

## Permissions

- **Internet**: provider, search, MCP, update, rating, and remote HTTP connections.
- **Notifications / foreground service**: ongoing generation, automation, and user-visible completion behavior.
- **Files and media**: only when you select/import attachments, models, backups, fonts, or shared sandbox storage.
- **Exact alarms**: optional automation scheduling where supported and explicitly enabled.

## Children, changes, and contact

Agora is not directed to children under 13. This policy may be updated with the repository/application. Questions can be opened at [github.com/newo-ether/Agora](https://github.com/newo-ether/Agora).

---

# 隐私政策

**最后更新：2026 年 8 月 10 日**

Agora 是 BYOK Android 客户端，不运营聊天补全中转服务：模型请求由设备直接发送到你配置的提供商或端点。

## 本地数据

对话、消息树、任务、循环、记忆、提示词、附件、工具媒体、设置和导入模型保存在设备上的应用管理数据库/文件中。机密设置通常使用 Android Keystore 支持的 AES-256-GCM 封装。旧值可作为明文读取，且加密失败时会为避免丢失数据而回退为明文，因此设备存储保护仍然重要。

清除应用数据或卸载会移除应用管理数据，除非 Android 备份或用户导出文件仍保留副本。

## 网络目的地

只有使用相应功能时，数据才会离开设备：

- 消息和附件发往所选 AI 提供商；
- 标题、转录、图片生成和嵌入请求发往各自所选提供商；
- 搜索查询发往所选网络搜索服务；
- MCP 调用发往已启用服务器；
- Conch/SSH 操作发往已配置远程设备；
- 应用启动时最多每天一次检查发布元数据；
- 可选评分表单只把你明确提交的评分、姓名、邮箱和评论发送到 `https://newoether.com/api/rating`；
- 崩溃后只在本地保存一份待处理报告，下次启动时询问是否发送到 `https://newoether.com/crash`。报告包含堆栈、应用/Android 版本、设备厂商/型号、时间戳和有界诊断事件，不含对话文本、凭据或设备标识符。

崩溃报告不会自动提交。Agora 不包含通用分析路径。第三方端点有各自的隐私与保留政策。

## 备份、传输与权限

`.agora` 是 ZIP 归档；若明确包含 API Key 或其他机密，这些值在归档内部未加密。网络代理只适用于共享 HTTP 客户端，不覆盖直接 SSH、本地推理或 Alpine 沙盒进程网络。Conch 的应用层加密需要 API Key；空 Key 端点使用明文 JSON，只依赖 HTTPS 保护传输。

应用可能按功能使用网络、通知/前台服务、用户选择的文件与媒体，以及明确启用时的精确闹钟权限。

## 儿童、变更与联系

Agora 不面向 13 岁以下儿童。本政策会随仓库/应用更新。如有问题，请在 [github.com/newo-ether/Agora](https://github.com/newo-ether/Agora) 提交 Issue。
