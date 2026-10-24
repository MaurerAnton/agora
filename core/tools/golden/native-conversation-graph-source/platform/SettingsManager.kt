// Storage boundary for the secrets policy. The real SettingsManager is a DataStore-backed Android
// class; the policy only reads seven flows and calls seven save methods, so this in-memory double
// preserves exactly that contract. No policy logic lives here.
package com.newoether.agora.data

import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow

class SettingsManager(
    apiKeys: List<ApiKeyEntry> = emptyList(),
    activeApiKeyIds: Map<String, String> = emptyMap(),
    webSearchApiKeys: Map<String, String> = emptyMap(),
    proxyPassword: String = "",
    shellDevices: List<ShellDeviceConfig> = emptyList(),
    embeddingModels: List<EmbeddingModelConfig> = emptyList(),
    mcpServers: List<McpServerConfig> = emptyList(),
) {
    private val apiKeysState = MutableStateFlow(apiKeys)
    private val activeApiKeyIdsState = MutableStateFlow(activeApiKeyIds)
    private val webSearchApiKeysState = MutableStateFlow(webSearchApiKeys)
    private val proxyPasswordState = MutableStateFlow(proxyPassword)
    private val shellDevicesState = MutableStateFlow(shellDevices)
    private val embeddingModelsState = MutableStateFlow(embeddingModels)
    private val mcpServersState = MutableStateFlow(mcpServers)

    val apiKeys: Flow<List<ApiKeyEntry>> = apiKeysState
    val activeApiKeyIds: Flow<Map<String, String>> = activeApiKeyIdsState
    val webSearchApiKeys: Flow<Map<String, String>> = webSearchApiKeysState
    val proxyPassword: Flow<String> = proxyPasswordState
    val shellDevices: Flow<List<ShellDeviceConfig>> = shellDevicesState
    val embeddingModels: Flow<List<EmbeddingModelConfig>> = embeddingModelsState
    val mcpServers: Flow<List<McpServerConfig>> = mcpServersState

    suspend fun saveApiKeys(keys: List<ApiKeyEntry>) {
        apiKeysState.value = keys
    }

    suspend fun saveActiveApiKeyIds(ids: Map<String, String>) {
        activeApiKeyIdsState.value = ids
    }

    suspend fun saveWebSearchApiKeys(keys: Map<String, String>) {
        webSearchApiKeysState.value = keys
    }

    suspend fun saveProxyPassword(pass: String) {
        proxyPasswordState.value = pass
    }

    suspend fun saveShellDevices(devices: List<ShellDeviceConfig>) {
        shellDevicesState.value = devices
    }

    suspend fun saveEmbeddingModels(models: List<EmbeddingModelConfig>) {
        embeddingModelsState.value = models
    }

    suspend fun saveMcpServers(servers: List<McpServerConfig>) {
        mcpServersState.value = servers
    }
}
