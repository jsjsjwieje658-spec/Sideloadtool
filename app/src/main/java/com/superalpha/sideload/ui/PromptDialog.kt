package com.superalpha.sideload.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import com.superalpha.sideload.bridge.UiPrompt

/**
 * Top-level dialog (mounted once, above the bottom-nav host) that surfaces whatever
 * Python is currently blocking on via [UiPrompt.requestInput] — a 2FA code, the
 * "drop extensions?" question, or a DSID. Shown regardless of which tab is active.
 *
 * v49: the question is shown as body text (it used to be the label of a single-line
 * field, so multi-line questions such as the extension prompt were clipped and
 * unreadable); 2FA prompts get a numeric keyboard; "Huỷ" sends an empty answer
 * (= cancel / skip on the Python side). Submissions use UiPrompt.submitResponse(),
 * which never blocks the main thread.
 */
@Composable
fun PromptDialogHost() {
    val promptText by UiPrompt.prompt.collectAsState()
    val text = promptText ?: return
    var input by remember(text) { mutableStateOf("") }
    val isCode = remember(text) { text.contains("2fa", ignoreCase = true) }

    AlertDialog(
        onDismissRequest = { /* Python is blocked waiting; do not allow silent dismiss. */ },
        title = { Text(if (isCode) "Xác thực 2 yếu tố" else "Cần nhập thông tin") },
        text = {
            Column(
                modifier = Modifier.verticalScroll(rememberScrollState()),
                verticalArrangement = Arrangement.spacedBy(12.dp)
            ) {
                Text(text.trim())
                OutlinedTextField(
                    value = input,
                    onValueChange = { v ->
                        input = if (isCode) v.filter { it.isDigit() }.take(6) else v
                    },
                    placeholder = { Text(if (isCode) "6 chữ số" else "Nhập câu trả lời") },
                    singleLine = true,
                    keyboardOptions = KeyboardOptions(
                        keyboardType = if (isCode) KeyboardType.Number else KeyboardType.Text
                    ),
                    modifier = Modifier.fillMaxWidth()
                )
            }
        },
        confirmButton = {
            TextButton(onClick = { UiPrompt.submitResponse(input.trim()) }) {
                Text("Gửi")
            }
        },
        dismissButton = {
            TextButton(onClick = { UiPrompt.submitResponse("") }) {
                Text("Huỷ")
            }
        }
    )
}
