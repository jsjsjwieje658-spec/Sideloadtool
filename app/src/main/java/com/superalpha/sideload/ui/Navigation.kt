package com.superalpha.sideload.ui

import androidx.compose.animation.EnterTransition
import androidx.compose.animation.ExitTransition
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.CloudUpload
import androidx.compose.material.icons.filled.Fingerprint
import androidx.compose.material.icons.filled.Link
import androidx.compose.material.icons.filled.PhoneAndroid
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material3.Icon
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.navigation.NavDestination.Companion.hierarchy
import androidx.navigation.NavGraph.Companion.findStartDestination
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.currentBackStackEntryAsState
import androidx.navigation.compose.rememberNavController

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

@Composable
fun AppNavHost(viewModel: HomeViewModel) {
    val navController = rememberNavController()

    Scaffold(
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
            modifier = androidx.compose.ui.Modifier.padding(innerPadding),
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
