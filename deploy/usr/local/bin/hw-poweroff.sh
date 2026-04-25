#!/bin/sh
# Hardware poweroff for Arch Linux (systemd)
#
# On systemd, the correct way to power off is through systemd's own shutdown path:
#   systemctl poweroff
#   → systemd stops all services in dependency order
#   → systemd calls reboot(LINUX_REBOOT_CMD_POWER_OFF) as PID 1
#   → kernel_power_off() → pm_power_off() → gpio_poweroff_do_poweroff()
#   → GPIO PE2: HIGH→100ms→LOW→100ms→HIGH→3000ms → PMIC power off
#
# WARNING: Do NOT use "kill -TERM -1" on systemd systems!
# On BusyBox init, kill -1 is safe because BusyBox init (PID 1) ignores it.
# On systemd, kill -1 causes mass service death, which can trigger systemd's
# emergency/watchdog reboot logic BEFORE reboot -f -p executes.

exec systemctl poweroff
