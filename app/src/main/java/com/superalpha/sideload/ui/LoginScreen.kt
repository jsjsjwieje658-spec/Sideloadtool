package com.superalpha.sideload.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.AccountCircle
import androidx.compose.material.icons.filled.RocketLaunch
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import com.superalpha.sideload.ui.theme.BrandTextDim
import com.superalpha.sideload.ui.theme.screenBackgroundBrush

/*
 * ════════════════════════════════════════════════════════════════════════
 *  v51 — LoginScreen: cổng đăng nhập Apple ID.
 *
 *  Hiện khi: (a) lần đầu cài app, hoặc (b) sau khi Đăng xuất trong Cài đặt
 *  (MainActivity quan sát HomeViewModel.signedIn để đổi sang màn này).
 *
 *  Người dùng nhập Apple ID + mật khẩu MỘT LẦN → app xác thực với Apple
 *  (PythonBridge.login → sideload_core.do_login; 2FA nếu Apple hỏi sẽ hiện
 *  PromptDialogHost — mounted ở trên cả màn này) → lưu lại (AppConfig) →
 *  vào app chính. Từ đó các màn Cài IPA / Thu hồi cert dùng luôn thông tin
 *  đã lưu, không cần nhập lại.
 *
 *  Nút "Lưu mà không xác thực": cho mạng/Anisette chập chờn — lưu thông tin
 *  rồi vào app (sai mật khẩu sẽ bị Apple báo ở lần ký đầu tiên).
 * ════════════════════════════════════════════════════════════════════════
 */
@Composable
fun LoginScreen(viewModel: HomeViewModel) {
    val busy by viewModel.busy.collectAsState()
    val busyText by viewModel.busyText.collectAsState()
    val savedAppleId by viewModel.savedAppleId.collectAsState()

    var appleId by remember { mutableStateOf("") }
    var appleIdPrefilled by remember { mutableStateOf(false) }
    var password by remember { mutableStateOf("") }

    // Tự điền Apple ID nếu từng lưu (vd bản cũ chỉ lưu email)
    LaunchedEffect(savedAppleId) {
        if (!appleIdPrefilled && savedAppleId.isNotBlank()) {
            if (appleId.isBlank()) appleId = savedAppleId
            appleIdPrefilled = true
        }
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(screenBackgroundBrush())
            .verticalScroll(rememberScrollState())
            .imePadding()
            .padding(horizontal = 20.dp),
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        Spacer(Modifier.height(48.dp))

        // Logo + tên app
        Box(
            modifier = Modifier
                .size(56.dp)
                .background(MaterialTheme.colorScheme.primaryContainer, RoundedCornerShape(17.dp)),
            contentAlignment = Alignment.Center
        ) {
            Icon(
                Icons.Filled.RocketLaunch,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.primary,
                modifier = Modifier.size(30.dp)
            )
        }
        Spacer(Modifier.height(12.dp))
        Text("SUPER ALPHA Sideload", style = MaterialTheme.typography.titleLarge)
        Text(
            "Cài IPA lên iPhone bằng Apple ID miễn phí",
            style = MaterialTheme.typography.bodySmall,
            color = BrandTextDim
        )

        Spacer(Modifier.height(24.dp))

        SectionCard(title = "Đăng nhập Apple ID", icon = Icons.Filled.AccountCircle) {
                Text(
                    "Nhập Apple ID lần đầu — app sẽ lưu riêng tư trên máy của bạn để " +
                        "các lần ký & cài đặt sau không phải nhập lại.",
                    style = MaterialTheme.typography.bodySmall,
                    color = BrandTextDim
                )
                Spacer(Modifier.height(12.dp))
                AppTextField(
                    value = appleId,
                    onValueChange = { appleId = it },
                    label = "Apple ID (email)",
                    keyboardType = KeyboardType.Email
                )
                Spacer(Modifier.height(10.dp))
                AppTextField(
                    value = password,
                    onValueChange = { password = it },
                    label = "Mật khẩu Apple ID",
                    password = true,
                    supportingText = "Nếu tài khoản bật 2FA, app sẽ hỏi mã ngay dưới đây."
                )
            }

        Spacer(Modifier.height(16.dp))

        PrimaryButton(
            text = "Đăng nhập",
            onClick = { viewModel.signIn(appleId, password) },
            enabled = !busy && appleId.isNotBlank() && password.isNotBlank(),
            busy = busy,
            busyText = busyText.ifBlank { "Đang xử lý…" }
        )

        Spacer(Modifier.height(6.dp))

        Box(modifier = Modifier.fillMaxWidth(), contentAlignment = Alignment.Center) {
            TextButton(
                onClick = { viewModel.saveCredentials(appleId, password) },
                enabled = !busy && appleId.isNotBlank() && password.isNotBlank()
            ) {
                Text(
                    "Lưu mà không xác thực (khi mạng lỗi)",
                    style = MaterialTheme.typography.labelMedium,
                    color = BrandTextDim
                )
            }
        }

        Spacer(Modifier.height(16.dp))

        LogConsole(Modifier.height(220.dp))

        Spacer(Modifier.height(24.dp))
    }
}
