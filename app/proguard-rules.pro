# Keep Chaquopy's Java<->Python bridge classes and our own bridge singletons,
# since Python calls into them by reflection-free direct JNI binding but the
# class/method names must survive R8 shrinking & obfuscation intact.
-keep class com.chaquo.python.** { *; }
-keep class com.superalpha.sideload.bridge.** { *; }

# Chaquopy ships its own consumer rules, but keeping this explicit avoids
# surprises if consumer rule merging ever misbehaves in CI.
-dontwarn com.chaquo.python.**
