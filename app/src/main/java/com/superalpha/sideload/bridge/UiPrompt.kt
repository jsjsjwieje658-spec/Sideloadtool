package com.superalpha.sideload.bridge

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import java.util.concurrent.SynchronousQueue

object UiPrompt {
    private val _prompt = MutableStateFlow<String?>(null)
    val prompt = _prompt.asStateFlow()
    private val responseQueue = SynchronousQueue<String>()

    /**
     * Gọi từ luồng nền Python (sideload_core._ui_input → mã 2FA, câu hỏi bỏ
     * extension, DSID). Hiện PromptDialogHost và CHẶN tới khi người dùng gửi.
     */
    @JvmStatic
    fun requestInput(promptText: String): String {
        _prompt.value = promptText
        try {
            return responseQueue.take()
        } finally {
            _prompt.value = null
        }
    }

    /**
     * Gửi câu trả lời cho luồng Python đang chờ trong [requestInput].
     *
     * BUGFIX v49: trước đây dùng put() — chạy trên MAIN thread, nếu không có
     * luồng nào đang chờ (bấm "Gửi" 2 lần trước khi hộp thoại kịp đóng) thì
     * put() chặn main thread vĩnh viễn → ANR, hoặc giá trị thừa bị dùng làm
     * câu trả lời cho câu hỏi SAU. offer() không chặn: chỉ trao khi Python
     * đang chờ, ngược lại trả false.
     */
    fun submitResponse(value: String): Boolean = responseQueue.offer(value)

    // Trust banner (NEW)
    private val _trustBanner = MutableStateFlow<String?>(null)
    val trustBanner = _trustBanner.asStateFlow()

    fun showTrustBanner(message: String?) { _trustBanner.value = message }
    fun dismissTrustBanner() { _trustBanner.value = null }
}
