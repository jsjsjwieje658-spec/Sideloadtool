package com.superalpha.sideload.ui

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
import com.superalpha.sideload.bridge.UiPrompt

/**
 * Top-level dialog (mounted once, above the bottom-nav host) that surfaces whatever
 * Python is currently blocking on via [UiPrompt.requestInput] — almost always a 2FA
 * code. Shown regardless of which of the three tabs is active, since the sideload
 * flow that triggers it can be started from either the Sideload or Revoke Certs tab.
 */
@Composable
fun PromptDialogHost() {
    val promptText by UiPrompt.prompt.collectAsState()
    val text = promptText ?: return
    var input by remember(text) { mutableStateOf("") }

    AlertDialog(
        onDismissRequest = { /* Python is blocked waiting; do not allow silent dismiss. */ },
        title = { Text("Cần nhập thông tin") },
        text = {
            OutlinedTextField(
                value = input,
                onValueChange = { input = it },
                label = { Text(text) },
                singleLine = true
            )
        },
        confirmButton = {
            TextButton(onClick = { UiPrompt.submitResponse(input) }) {
                Text("Gửi")
            }
        }
    )
}
