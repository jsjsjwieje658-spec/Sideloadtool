package com.superalpha.sideload.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.superalpha.sideload.ui.theme.BrandText

/** Scrolling monospace log console shared by the Sideload and Revoke Certs screens. */
@Composable
fun LogConsole(lines: List<String>, modifier: Modifier = Modifier) {
    val listState = rememberLazyListState()
    // `mounted` bắt đầu lại từ false mỗi lần Composable này được TẠO MỚI — điều
    // này xảy ra mỗi khi chuyển sang tab khác rồi quay lại (NavHost bỏ hẳn nội
    // dung tab cũ khỏi composition). Trước đây animateScrollToItem() luôn được
    // gọi kể cả ở lần mount đầu tiên này, nên mỗi lần quay lại tab Sideload/Thu
    // hồi cert, log (có thể tới 500 dòng) lại cuộn CÓ ANIMATION từ đầu xuống
    // cuối — đây chính là cảm giác "trễ" khi đổi tab. Sửa: lần mount đầu tiên
    // nhảy thẳng tới cuối (không animation); chỉ animate khi có dòng log MỚI
    // xuất hiện thật trong lúc màn hình đang mở.
    var mounted by remember { mutableStateOf(false) }
    LaunchedEffect(lines.size) {
        if (lines.isEmpty()) return@LaunchedEffect
        if (!mounted) {
            listState.scrollToItem(lines.size - 1)
            mounted = true
        } else {
            listState.animateScrollToItem(lines.size - 1)
        }
    }
    Box(
        modifier = modifier
            .fillMaxWidth()
            .background(androidx.compose.ui.graphics.Color(0xFF05080B))
            .padding(8.dp)
    ) {
        LazyColumn(state = listState, modifier = Modifier.fillMaxSize(), verticalArrangement = Arrangement.Top) {
            itemsIndexed(lines) { _, line ->
                Text(
                    text = line,
                    color = BrandText,
                    fontFamily = FontFamily.Monospace,
                    fontSize = 11.sp,
                    modifier = Modifier.padding(vertical = 1.dp)
                )
            }
        }
    }
}
