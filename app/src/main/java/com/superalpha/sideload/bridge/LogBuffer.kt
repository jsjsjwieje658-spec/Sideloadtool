package com.superalpha.sideload.bridge

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch

/*
 * ════════════════════════════════════════════════════════════════════════
 *  v50 — LogBuffer: ring buffer + gộp dòng theo lô (batch) cho LogConsole.
 *
 *  VẤN ĐỀ CŨ (nguyên nhân UI lag khi log dồn dập):
 *    HomeViewModel giữ `MutableStateFlow<List<String>>` và với MỖI dòng log
 *    (Python/native phát hàng chục dòng mỗi giây khi chép IPA) lại:
 *      1. copy toàn bộ List (tới 500 phần tử) + `.takeLast(500)` tạo list mới,
 *      2. phát StateFlow → recompose Toàn Bộ màn hình đang collect `log`,
 *      3. LazyColumn không có key → mọi item đang hiển thị được compose lại,
 *      4. LogConsole `animateScrollToItem()` chạy animation cuộn liên tục.
 *    Trong lúc [afc]/[instproxy] báo %, điều này xảy ra ~10–30 lần/giây →
 *    giật khung hình rõ rệt trên máy thật (debug build còn nặng hơn).
 *
 *  GIẢI PHÁP:
 *    - Dòng log mới chỉ được APPEND vào deque (O(1), không copy) dưới lock.
 *    - Một coroutine flush duy nhất gộp các dòng rồi xuất Snapshot MỘT LẦN
 *      mỗi 100 ms — UI nhận tối đa 10 bản cập nhật/giây bất kể log dày thế nào.
 *    - Ring buffer giới hạn 2000 dòng (đủ cho toàn bộ quá trình sideload;
 *      nếu tràn thì bỏ dòng cũ nhất, firstIndex vẫn tăng dần để key của
 *      LazyColumn luôn ổn định — item cũ không bị compose lại sai).
 *    - `clear()` xoá cả pending lẫn committed để nút "Xoá" phản hồi tức thì.
 * ════════════════════════════════════════════════════════════════════════
 */
object LogBuffer {

    /** Ảnh chụp bất biến của console: các dòng + chỉ số của dòng đầu tiên. */
    class Snapshot(val lines: List<String>, val firstIndex: Long) {
        /** Chỉ số (toàn cục, đơn điệu tăng) của dòng kế tiếp sau snapshot. */
        val nextIndex: Long get() = firstIndex + lines.size
        val lastIndex: Long get() = nextIndex - 1
    }

    private const val MAX_LINES = 2000
    private const val FLUSH_INTERVAL_MS = 100L

    private val lock = Any()
    private val pending = ArrayDeque<String>()
    private val committed = ArrayDeque<String>(MAX_LINES)
    private var totalAppended = 0L
    private var dirty = false

    private val _snapshot = MutableStateFlow(Snapshot(emptyList(), 0L))
    val snapshot: StateFlow<Snapshot> = _snapshot.asStateFlow()

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)

    init {
        scope.launch {
            while (isActive) {
                delay(FLUSH_INTERVAL_MS)
                if (dirty) flush()
            }
        }
    }

    /** Được NativeLog.emit() gọi từ mọi luồng (UI, Python, JNI, IO). Không chặn. */
    fun append(line: String) {
        synchronized(lock) {
            pending.addLast(line)
            dirty = true
        }
    }

    private fun flush() {
        val snap = synchronized(lock) {
            if (!dirty) return
            while (pending.isNotEmpty()) {
                committed.addLast(pending.removeFirst())
                if (committed.size > MAX_LINES) committed.removeFirst()
            }
            dirty = false
            Snapshot(committed.toList(), totalAppended - committed.size)
        }
        _snapshot.value = snap
    }

    /** Xoá toàn bộ log — gọi từ nút "Xoá log" trên UI. */
    fun clear() {
        synchronized(lock) {
            pending.clear()
            committed.clear()
            dirty = false
        }
        _snapshot.value = Snapshot(emptyList(), totalAppended)
    }

    /** Toàn bộ log hiện tại dạng văn bản — dùng cho nút "Sao chép". */
    fun copyText(): String = synchronized(lock) {
        buildString {
            committed.forEach { appendLine(it) }
            pending.forEach { appendLine(it) }
        }
    }
}
