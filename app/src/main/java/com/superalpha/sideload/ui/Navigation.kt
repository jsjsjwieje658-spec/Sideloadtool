package com.superalpha.sideload.ui

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.EnterTransition
import androidx.compose.animation.ExitTransition
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutVertically
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.CloudUpload
import androidx.compose.material.icons.filled.Fingerprint
import androidx.compose.material.icons.filled.Link
import androidx.compose.material.icons.filled.PhoneAndroid
import androidx.compose.material.icons.filled.PhoneIphone
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material3.Button
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import androidx.navigation.NavDestination.Companion.hierarchy
import androidx.navigation.NavGraph.Companion.findStartDestination
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.currentBackStackEntryAsState
import androidx.navigation.compose.rememberNavController
import com.superalpha.sideload.bridge.NativeBridge

private sealed class Screen(val route: String, val label: String, val icon: androidx.compose.ui.graphics.vector.ImageVector) {
    object Sideload : Screen("sideload", "Cài IPA", Icons.Filled.CloudUpload)
    object Pairing : Screen("pairing", "Ghép nối", Icons.Filled.Link)
    object RegisterDevice : Screen("register_device", "Đăng ký UDID", Icons.Filled.Fingerprint)
    object Revoke : Screen("revoke", "Thu hồi cert", Icons.Filled.PhoneAndroid)
    object Settings : Screen("settings", "Cài đặt", Icons.Filled.Settings)
}

// BUGFIX v15: thêm tab "Đăng ký UDID" — trước đây đăng ký UDID chỉ có
// thể xảy ra ngầm bên trong luồng "Cài IPA" (SideloadScreen), không có cách
// nào đăng ký UDID độc lập với việc ký/cài một IPA cụ thể.
//
// [MỚI] thêm tab "Ghép nối" — cho phép tạo pairing file và ghép nối với
// iPhone độc lập với luồng Cài IPA (theo yêu cầu người dùng), đồng thời giúp
// kiểm tra riêng bước bắt tay usbmux/Trust khi gặp lỗi kết nối.
private val screens = listOf(Screen.Sideload, Screen.Pairing, Screen.RegisterDevice, Screen.Revoke, Screen.Settings)

/**
 * FIX v28: TrustBanner — Hiển thị toàn màn hình khi iPhone yêu cầu Trust.
 *
 * VẤN ĐỀ CŨ: C code gọi NativeBridge.onTrustRequired() → set _trustRequired
 * StateFlow, nhưng KHÔNG CÓ composable nào observe flow này để hiện UI.
 * Kết quả: người dùng không biết phải bấm Trust trên iPhone → wait loop
 * trong nativePair() timeout sau 20 lần × 2 giây = 40 giây → thất bại.
 *
 * FIX: Banner này mount ở cấp AppNavHost (phía trên NavHost, dưới bottomBar)
 * → hiện ở MỌI tab, không chỉ PairingScreen. Người dùng thấy ngay dù đang
 * ở bất kỳ màn hình nào. Banner animate slideIn từ trên, tự dismiss khi
 * NativeBridge.dismissTrust() được gọi (sau khi Trust được xác nhận).
 */
@Composable
private fun TrustBanner(viewModel: HomeViewModel) {
    val trustRequired by viewModel.trustRequired.collectAsState()

    AnimatedVisibility(
        visible = trustRequired,
        enter = slideInVertically(initialOffsetY = { -it }),
        exit  = slideOutVertically(targetOffsetY  = { -it })
    ) {
        Box(
            modifier = Modifier
                .fillMaxWidth()
                .background(Color(0xFFE65100))  /* Deep orange — chú ý cao */
                .padding(horizontal = 16.dp, vertical = 10.dp)
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Icon(
                    Icons.Filled.PhoneIphone,
                    contentDescription = null,
                    tint = Color.White
                )
                Spacer(Modifier.width(10.dp))
                Column(modifier = Modifier.weight(1f)) {
                    Text(
                        "Xác nhận 'Tin cậy' trên iPhone!",
                        color = Color.White,
                        style = MaterialTheme.typography.titleSmall
                    )
                    Text(
                        "Mở khoá iPhone → bấm 'Tin cậy Máy tính này' → nhập mã PIN",
                        color = Color.White.copy(alpha = 0.9f),
                        style = MaterialTheme.typography.bodySmall
                    )
                }
                Spacer(Modifier.width(8.dp))
                Button(onClick = { viewModel.dismissTrust() }) {
                    Text("Đã bấm", color = Color.White)
                }
            }
        }
    }
}

@Composable
fun AppNavHost(viewModel: HomeViewModel) {
    val navController = rememberNavController()

    Scaffold(
        topBar = {
            /* FIX v28: Trust banner dưới top — hiện ở MỌI tab, không chỉ Pairing */
            TrustBanner(viewModel)
        },
        bottomBar = {
            NavigationBar {
                val navBackStackEntry by navController.currentBackStackEntryAsState()
                val currentDestination = navBackStackEntry?.destination
                screens.forEach { screen ->
                    NavigationBarItem(
                        icon = { Icon(screen.icon, contentDescription = screen.label) },
                        label = { Text(screen.label) },
                        selected = currentDestination?.hierarchy?.any { it.route == screen.route } == true,
                        onClick = {
                            navController.navigate(screen.route) {
                                popUpTo(navController.graph.findStartDestination().id) { saveState = true }
                                launchSingleTop = true
                                restoreState = true
                            }
                        }
                    )
                }
            }
        }
    ) { innerPadding ->
        NavHost(
            navController = navController,
            startDestination = Screen.Sideload.route,
            modifier = Modifier.padding(innerPadding),
            // Bỏ hoàn toàn animation chuyển màn khi đổi tab dưới cùng: đây là
            // điều hướng ngang cấp (3 tab chính), không phải push/pop kiểu
            // "đi sâu vào màn hình con", nên animation trượt mặc định của
            // Compose Navigation chỉ tạo cảm giác trễ mà không có giá trị điều
            // hướng nào — chuyển tab giờ đổi nội dung ngay lập tức.
            enterTransition = { EnterTransition.None },
            exitTransition = { ExitTransition.None },
            popEnterTransition = { EnterTransition.None },
            popExitTransition = { ExitTransition.None }
        ) {
            composable(Screen.Sideload.route) { SideloadScreen(viewModel) }
            composable(Screen.Pairing.route) { PairingScreen(viewModel) }
            composable(Screen.RegisterDevice.route) { RegisterDeviceScreen(viewModel) }
            composable(Screen.Revoke.route) { RevokeCertsScreen(viewModel) }
            composable(Screen.Settings.route) { SettingsScreen(viewModel) }
        }
    }
}
