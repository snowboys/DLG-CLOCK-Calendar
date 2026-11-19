set confirm off
set pagination off
set architecture arm
target extended-remote 127.0.0.1:2331
monitor halt
restore D:/SDK_6.0.22.1401/DA145xx_SDK/6.0.22.1401/projects/target_apps/ble_examples/pcmonitor/build/pcmonitor.bin binary 0x07FC0000
set $sp = 0x07FD21BC
set $pc = 0x07FC0511
monitor go
# 保持调试会话存活, 让目标在 J-Link 下自由运行
shell ping -n 8 127.0.0.1 > nul
quit
