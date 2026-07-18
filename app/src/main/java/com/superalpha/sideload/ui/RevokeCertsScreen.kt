package com.superalpha.sideload.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import com.superalpha.sideload.bridge.NativeLog
import com.superalpha.sideload.python.PythonBridge
import kotlinx.coroutines.launch

/**
 * Mirrors revoke_certs.py's interactive flow: log in, list existing development
 * certificates for the team, then revoke either one by index or all of them (needed
 * because Apple free/personal accounts are limited to 2 active certs at a time).
 */
@Composable
fun RevokeCertsScreen(viewModel: HomeViewModel) {
    val scope = rememberCoroutineScope()
    val logLines by viewModel.log.collectAsState()
    val busy by viewModel.busy.collectAsState()
    val savedAppleId by viewModel.savedAppleId.collectAsState()
    val savedAnisetteUrl by viewModel.savedAnisetteUrl.collectAsState()

    var appleId by remember { mutableStateOf("") }
    var appleIdPrefilled by remember { mutableStateOf(false) }
    var password by remember { mutableStateOf("") }
    var certSelector by remember { mutableStateOf("all") }

    androidx.compose.runtime.LaunchedEffect(savedAppleId) {
        if (!appleIdPrefilled && savedAppleId.isNotBlank()) {
            if (appleId.isBlank()) appleId = savedAppleId
            appleIdPrefilled = true
        }
    }

    Column(modifier = Modifier.fillMaxSize().padding(16.dp)) {
        Text("Thu hồi chứng chỉ ký (certificate)", style = MaterialTheme.typography.titleLarge)
        Spacer(Modifier.height(8.dp))
        Text(
            "Tài khoản Apple ID miễn phí chỉ được phép có tối đa 2 chứng chỉ Development đang hoạt động. " +
                "Thu hồi chứng chỉ cũ khi bạn gặp lỗi giới hạn số lượng.",
            style = MaterialTheme.typography.bodySmall
        )

        Spacer(Modifier.height(16.dp))
        OutlinedTextField(value = appleId, onValueChange = { appleId = it }, label = { Text("Apple ID") }, modifier = Modifier.fillMaxWidth())
        Spacer(Modifier.height(8.dp))
        OutlinedTextField(
            value = password, onValueChange = { password = it },
            label = { Text("Mật khẩu Apple ID") },
            visualTransformation = PasswordVisualTransformation(),
            modifier = Modifier.fillMaxWidth()
        )
        Spacer(Modifier.height(8.dp))
        OutlinedTextField(
            value = certSelector, onValueChange = { certSelector = it },
            label = { Text("Chỉ số chứng chỉ cần thu hồi (hoặc \"all\")") },
            modifier = Modifier.fillMaxWidth()
        )

        Spacer(Modifier.height(16.dp))
        Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
            Button(
                enabled = !busy && appleId.isNotBlank() && password.isNotBlank(),
                onClick = {
                    viewModel.setBusy(true)
                    scope.launch {
                        NativeLog.log("Đang đăng nhập & tra cứu chứng chỉ...")
                        val outcome = PythonBridge.revokeCerts(
                            appleId, password, savedAnisetteUrl.ifBlank { null }, certSelector
                        )
                        if (!outcome.success && outcome.message.isNotBlank()) {
                            NativeLog.log("Lỗi: ${outcome.message}")
                        }
                        viewModel.setBusy(false)
                    }
                }
            ) {
                if (busy) {
                    CircularProgressIndicator(modifier = Modifier.height(16.dp), strokeWidth = 2.dp)
                } else {
                    Text("Thu hồi")
                }
            }
            TextButton(onClick = { viewModel.clearLog() }) { Text("Xoá log") }
        }

        Spacer(Modifier.height(12.dp))
        Text("Nhật ký:", style = MaterialTheme.typography.labelLarge)
        Spacer(Modifier.height(4.dp))
        LogConsole(lines = logLines, modifier = Modifier.weight(1f))
    }
}
