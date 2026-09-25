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
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Key
import androidx.compose.material.icons.filled.PhoneIphone
import androidx.compose.material.icons.filled.RocketLaunch
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
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.unit.dp
import androidx.navigation.NavDestination.Companion.hierarchy
import androidx.navigation.NavGraph.Companion.findStartDestination
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.currentBackStackEntryAsState
import androidx.navigation.compose.rememberNavController
import com.superalpha.sideload.bridge.NativeBridge

/*
 * ════════════════════════════════════════════════════════════════════════
 *  v50 — Navigation (thiết kế lại).
 *
 *  BỎ 2 tab theo yêu cầu: "Ghép nối" và "Đăng ký UDID". Cả hai việc đó
 *  đều diễn ra NGẦM trong luồng "Cài IPA" (do_sideload Bước 0/5: connect →
 *  pair → đăng ký UDID), còn trạng thái tương ứng hiển thị ngay trên thẻ
 *  iPhone ở màn chính (DeviceCard).
 *
 *  Còn 3 tab: Cài IPA · Thu hồi cert · Cài đặt.
 *
 *  Giữ các fix cũ: banner Trust toàn cục (v28) và bỏ animation chuyển tab
 *  (EnterTransition.None) để đổi tab tức thì.
 * ════════════════════════════════════════════════════════════════════════
 */

private sealed class Screen(val route: String, val label: String, val icon: ImageVector) {
    object Sideload : Screen("sideload", "Cài IPA", Icons.Filled.RocketLaunch)
    object Revoke : Screen("revoke", "Thu hồi cert", Icons.Filled.Key)
    object Settings : Screen("settings", "Cài đặt", Icons.Filled.Settings)
}

private val screens = listOf(Screen.Sideload, Screen.Revoke, Screen.Settings)

/**
 * Banner toàn màn hình khi iPhone yêu cầu Trust — mount ở topBar nên hiện
 * ở MỌI tab (fix v28), tự ẩn khi NativeBridge.dismissTrust() được gọi.
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
                .background(Color(0xFF7A3E00))
        ) {
            Row(
                modifier = Modifier
                    .statusBarsPadding()
                    .padding(horizontal = 16.dp, vertical = 10.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                Icon(Icons.Filled.PhoneIphone, contentDescription = null, tint = Color.White)
                Spacer(Modifier.width(10.dp))
                Column(modifier = Modifier.weight(1f)) {
                    Text(
                        "Xác nhận \"Tin cậy\" trên iPhone!",
                        color = Color.White,
                        style = MaterialTheme.typography.titleSmall
                    )
                    Text(
                        "Mở khoá iPhone → bấm \"Tin cậy Máy tính này\" → nhập mã PIN",
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
        containerColor = MaterialTheme.colorScheme.background,
        topBar = { TrustBanner(viewModel) },
        bottomBar = {
            NavigationBar(
                containerColor = MaterialTheme.colorScheme.surface,
                tonalElevation = 0.dp
            ) {
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
            // Đổi tab là điều hướng ngang cấp — đổi nội dung ngay, không trượt.
            enterTransition = { EnterTransition.None },
            exitTransition = { ExitTransition.None },
            popEnterTransition = { EnterTransition.None },
            popExitTransition = { ExitTransition.None }
        ) {
            composable(Screen.Sideload.route) { SideloadScreen(viewModel) }
            composable(Screen.Revoke.route) { RevokeCertsScreen(viewModel) }
            composable(Screen.Settings.route) { SettingsScreen(viewModel) }
        }
    }
}
