package com.superalpha.sideload.bridge

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import java.util.concurrent.SynchronousQueue

object UiPrompt {
    private val _prompt = MutableStateFlow<String?>(null)
    val prompt = _prompt.asStateFlow()
    private val responseQueue = SynchronousQueue<String>()

    @JvmStatic
    fun requestInput(promptText: String): String {
        _prompt.value = promptText
        val value = responseQueue.take()
        _prompt.value = null
        return value
    }

    fun submitResponse(value: String) = responseQueue.put(value)

    // Trust banner (NEW)
    private val _trustBanner = MutableStateFlow<String?>(null)
    val trustBanner = _trustBanner.asStateFlow()

    fun showTrustBanner(message: String?) { _trustBanner.value = message }
    fun dismissTrustBanner() { _trustBanner.value = null }
}
