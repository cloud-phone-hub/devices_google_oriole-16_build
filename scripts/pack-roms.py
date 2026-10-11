#!/usr/bin/env python3
"""Pixel6-15 最终 ROM 打包编排。

两步任务：
  1) 将 7 个逻辑分区镜像合并成 super.img（liblp 动态分区，放大 super 到 6GiB）：
        odm                <- lineage-23/super/odm.img
        product            <- factory product.img
        system_ext         <- factory system_ext.img
        system_dlkm        <- factory system_other.img
        system             <- factory system.img（官方件 verbatim，system 重打包阶段未移植）
        vendor_dlkm        <- out/dist/rom/vendor_dlkm.img（重打包）
        vendor             <- out/dist/rom/vendor.img（重打包）
     产物 super.img 放到 out/dist/rom/。

  2) 将 super.img 与各物理分区镜像组装成 vda.img（GPT 磁盘，super 放大到 6GiB），
     并生成空 data.qcow2（110 GiB 稀疏种子，daemon 以可写 overlay 叠于其上）。

目标目录（daemon 运行盘集），保留 vda.img、data.qcow2 与 patch.img（vdc）：
  /Users/cuimingqiang/workspace/cloud-phone-hub-ws/out/device/pixel6/15/oriole-bp1a.250505.005/

用法：
  python3 build/pack-roms.py
  python3 build/pack-roms.py --force     # 强制重建 super/vda 并覆盖
  python3 build/pack-roms.py --clean     # 先清空目标目录
  python3 build/pack-roms.py --dry-run   # 只打印将执行的动作
"""

import argparse
import os
import shutil
import struct
import subprocess
import sys
import uuid
import zlib

import pack_super

from buildenv import env, PATCH_DIST

PIXEL6 = os.path.abspath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
)
OUT_ROOT = "/Users/cuimingqiang/workspace/cloud-phone-hub-ws/out"

# 版本/固件目录/lineage 目录由 CDH_VERSION、CDH_FACTORY_DIR、CDH_LINEAGE_DIR
# 环境变量优先（main.py --build-id 导出），独立运行回退当前唯一 ROM。
VERSION = env("CDH_VERSION", "oriole-cp1a.260405.005")
IMAGE_DIR = os.path.join(OUT_ROOT, "device", "oriole", "16", VERSION)

LINEAGE_DIR = env(
    "CDH_LINEAGE_DIR", "/Users/cuimingqiang/workspace/cloud-phone-hub-ws/lineage-rom/lineage-23"
)
ROMS_DIR = env("CDH_FACTORY_DIR", os.path.join(PIXEL6, "roms", f"image-{VERSION}"))
ROM_DIST = os.path.join(PIXEL6, "out", "dist", "rom")

# 自定义可引导 grub.cfg（与基线盘 ESP 一致：kms_display_resolution + gsifromvdc 默认 +
# 完整 oriole 硬件 cmdline）。装配 vda 时会覆写到 EFI 分区 /boot/grub/grub.cfg。
# 内容内联为常量以自包含（源 = 基线 cloud-droid-hub ESP grub.cfg.virtio-boot），
# 构建不再依赖 build/resources/ 资源目录。
GRUB_CFG_CONTENT = b"""\
echo CDH_GRUB_BANNER_9518
set kms_display_resolution=1080x2400
#
# Copyright (C) 2024 The LineageOS Project
#
# SPDX-License-Identifier: Apache-2.0
#

insmod normal

insmod gfxmenu
insmod gfxterm_background

insmod android_bootimg_fs
insmod echo
insmod eval
insmod linux
insmod loadenv
insmod loadstring
insmod reboot
insmod serial
insmod sleep
insmod test
insmod true

initrd_no_align_up

set ab_enable=false
export ab_enable

search.fs_label PERSIST partition_persist (hd0,gpt4) (hd0,gpt9) (hd1,gpt4) (hd1,gpt9) (hd2,gpt4) (hd2,gpt9)
set partition_persist="(${partition_persist})"
export partition_persist

# Match and set disk name using the ugly way, as there is no way to extract substring from string yet...
if [ "$partition_persist" = "(hd2,gpt4)" ]; then
	set disk_boot=hd2
elif [ "$partition_persist" = "(hd2,gpt9)" ]; then
	set disk_boot=hd2
elif [ "$partition_persist" = "(hd1,gpt4)" ]; then
	set disk_boot=hd1
elif [ "$partition_persist" = "(hd1,gpt9)" ]; then
	set disk_boot=hd1
else
	set disk_boot=hd0
fi
export disk_boot

if [ "$ab_enable" = "true" ]; then
	set partition_boot_a="(${disk_boot},gpt9)"
	set partition_boot_b="(${disk_boot},gpt10)"
	set partition_misc="(${disk_boot},gpt3)"
	set partition_vendor_boot_a="(${disk_boot},gpt7)"
	set partition_vendor_boot_b="(${disk_boot},gpt8)"
	export partition_boot_a
	export partition_boot_b
	export partition_vendor_boot_a
	export partition_vendor_boot_b
else
	set partition_boot="(${disk_boot},gpt6)"
	set partition_misc="(${disk_boot},gpt3)"
	set partition_recovery="(${disk_boot},gpt7)"
	set partition_vendor_boot="(${disk_boot},gpt11)"
	export partition_boot
	export partition_recovery
	export partition_vendor_boot
fi
export partition_misc

function set_kernel_cmdline_dynamic {
	load_env -f ${partition_persist}/grubenv android_insecure_adb android_lcd_density android_low_perf android_nobootanim android_selinux_permissive android_theme android_wifi_impl grub_timeout kms_display_resolution mitigations quiet

	set kernel_cmdline_dynamic=

	if [ "$android_insecure_adb" = "1" ]; then
		set kernel_cmdline_dynamic="${kernel_cmdline_dynamic} androidboot.insecure_adb=1"
	fi

	if [ "$android_lcd_density" ]; then
		set kernel_cmdline_dynamic="${kernel_cmdline_dynamic} androidboot.lcd_density=${android_lcd_density}"
	fi

	if [ "$android_low_perf" = "1" ]; then
		set kernel_cmdline_dynamic="${kernel_cmdline_dynamic} androidboot.low_perf=1"
	fi

	if [ "$android_nobootanim" = "1" ]; then
		set kernel_cmdline_dynamic="${kernel_cmdline_dynamic} androidboot.nobootanim=1"
	fi

	if [ "$android_selinux_permissive" = "1" ]; then
		set kernel_cmdline_dynamic="${kernel_cmdline_dynamic}"
	fi

	if [ "$android_theme" = "1" ]; then
		# Light
		set androidboot_theme=0
	elif [ "$android_theme" = "2" ]; then
		# Dark
		set androidboot_theme=1
	fi

	if [ "$android_wifi_impl" ]; then
		set kernel_cmdline_dynamic="${kernel_cmdline_dynamic} androidboot.wifi_impl=${android_wifi_impl}"
	fi

	if [ "$kms_display_resolution" ]; then
		set kernel_cmdline_dynamic="${kernel_cmdline_dynamic} virtio_gpu.force_resolution=${kms_display_resolution} vmwgfx.force_resolution=${kms_display_resolution}"
	fi

	if [ "$mitigations" = "off" ]; then
		set kernel_cmdline_dynamic="${kernel_cmdline_dynamic} mitigations=off"
	fi

	if [ "$quiet" = "1" ]; then
		set kernel_cmdline_dynamic="${kernel_cmdline_dynamic} quiet"
	fi

	if [ "$ab_enable" = "true" ]; then
		if [ "$abootctrl_global_active_slot" ]; then
			set kernel_cmdline_dynamic="${kernel_cmdline_dynamic}"
		fi
		if [ "$androidboot_mode" = "recovery" ]; then
			set kernel_cmdline_dynamic="${kernel_cmdline_dynamic} androidboot.mode=recovery"
		else
			set kernel_cmdline_dynamic="${kernel_cmdline_dynamic}"
		fi
	fi

	if [ "$kernel_cmdline_dynamic_device_specific" ]; then
		set kernel_cmdline_dynamic="${kernel_cmdline_dynamic} ${kernel_cmdline_dynamic_device_specific}"
	fi

	export kernel_cmdline_dynamic
}

function save_setting {
	set ${1}=${2}
	save_env -f ${partition_persist}/grubenv ${1}
}

function ab_load_needed_env {
	for global_item in active_slot; do
		load_env -f ${partition_persist}/grubenv_abootctrl abootctrl_global_${global_item}
		if [ "$?" != "0" ]; then
			return 1
		fi
		export abootctrl_global_${global_item}
	done
	for slot in $ab_slots; do
		for slot_item in boot_count is_bootable; do
			load_env -f ${partition_persist}/grubenv_abootctrl abootctrl_slot_${slot}_${slot_item}
			if [ "$?" != "0" ]; then
				return 1
			fi
			export abootctrl_slot_${slot}_${slot_item}
		done
	done
}
function ab_load_unneeded_env {
	for global_item in current_slot snapshot_merge_status no_auto_slot_switch; do
		load_env -f ${partition_persist}/grubenv_abootctrl abootctrl_global_${global_item}
		export abootctrl_global_${global_item}
	done
	for slot in $ab_slots; do
		for slot_item in is_successful; do
			load_env -f ${partition_persist}/grubenv_abootctrl abootctrl_slot_${slot}_${slot_item}
			export abootctrl_slot_${slot}_${slot_item}
		done
	done
}
function ab_set_save_env {
	set ${1}=${2}
	export ${1}
	save_env -f ${partition_persist}/grubenv_abootctrl ${1}
}
function ab_check_active_slot_boot_count {
	var_name=abootctrl_slot_${abootctrl_global_active_slot}_boot_count
	eval var_value=\\$$var_name
	if [ "$var_value" = "XXX" ]; then
		# Bad: Tried to boot for 3 times
		return 1
	else
		# Good
		return 0
	fi
}
function ab_update_active_slot_boot_count {
	var_name=abootctrl_slot_${abootctrl_global_active_slot}_boot_count
	if [ "$1" = "increase" ]; then
		eval var_value=\\$$var_name
		set_var_value=${var_value}X
	else
		set_var_value=
	fi
	ab_set_save_env $var_name $set_var_value
}
function ab_check_active_slot_is_bootable {
	var_name=abootctrl_slot_${abootctrl_global_active_slot}_is_bootable
	eval var_value=\\$$var_name
	if [ "$var_value" = "false" ]; then
		# Bad: Not bootable
		return 1
	else
		# Good
		return 0
	fi
}
function ab_set_active_slot_is_bootable {
	var_name=abootctrl_slot_${abootctrl_global_active_slot}_is_bootable
	if [ "$1" = "false" ]; then
		set_var_value=false
	else
		set_var_value=true
	fi
	ab_set_save_env $var_name $set_var_value
}
function ab_set_active_slot_is_successful {
	var_name=abootctrl_slot_${abootctrl_global_active_slot}_is_successful
	if [ "$1" = "false" ]; then
		set_var_value=false
	else
		set_var_value=true
	fi
	ab_set_save_env $var_name $set_var_value
}
function ab_set_boot_partition_var {
	var_name=partition_boot_$abootctrl_global_active_slot
	eval set boot_partition=\\$$var_name
	export boot_partition

	# A/B does not use recovery partition
	set recovery_partition=$boot_partition
	export recovery_partition

	var_name=partition_vendor_boot_$abootctrl_global_active_slot
	eval set vendor_boot_partition=\\$$var_name
	export vendor_boot_partition
}
function ab_load_active_slot {
	ab_set_boot_partition_var
	set_kernel_cmdline_dynamic
}
function ab_switch_active_slot {
	if [ "$abootctrl_global_no_auto_slot_switch" = "true" ]; then
		if [ "$1" != "manual" ]; then
			echo "WARNING: Skipping automatic slot switching as requested"
			return 1
		fi
	fi

	# Change active slot to another
	if [ "$abootctrl_global_active_slot" = "a" ]; then
		echo "Switching active slot to b..."
		ab_set_save_env abootctrl_global_active_slot b
	elif [ "$abootctrl_global_active_slot" = "b" ]; then
		echo "Switching active slot to a..."
		ab_set_save_env abootctrl_global_active_slot a
	else
		echo "Switching active slot to a..."
		ab_set_save_env abootctrl_global_active_slot a
	fi

	# Mark the new active slot as bootable
	ab_update_active_slot_boot_count clear
	ab_set_active_slot_is_bootable true

	ab_load_active_slot
}
function ab_init {
	set ab_slots="a b"
	export ab_slots
	if ! ab_load_needed_env; then
		set ab_init_failed=true
		export ab_init_failed
		# Fallback
		set abootctrl_global_active_slot=a
		export abootctrl_global_active_slot
		return 1
	fi
	ab_load_unneeded_env
	ab_set_boot_partition_var
}
function pre_boot_hook {
	if [ "$ab_enable" != "true" ]; then
		set_kernel_cmdline_dynamic
		return 0
	fi

	set androidboot_mode=$1
	ab_load_active_slot

	if [ "$ab_init_failed" = "true" ]; then
		echo "WARNING: A/B initialization has failed. Default to slot ${abootctrl_global_active_slot}."
		return 0
	fi

	if [ ! -f "${boot_partition}/kernel" ]; then
		# The boot partition according to the active slot is inaccessible
		echo "ERROR: Could not access kernel from active slot ${abootctrl_global_active_slot}."
		ab_set_active_slot_is_bootable false
		ab_switch_active_slot
	fi

	if ab_check_active_slot_is_bootable; then
		if ab_check_active_slot_boot_count; then
			# Best case: Active slot is bootable and boot retry count is small
			true # Do nothing
		else
			# Active slot's boot retry count exceeds but still marked as bootable
			echo "ERROR: Boot retry count of active slot ${abootctrl_global_active_slot} exceeds."
			ab_set_active_slot_is_bootable false
			ab_switch_active_slot
		fi
	else
		# Active slot is outright marked as not bootable
		echo "ERROR: Active slot ${abootctrl_global_active_slot} is marked as not bootable."
		ab_switch_active_slot
	fi

	# Let's boot the now active slot
	echo "INFO: Booting active slot ${abootctrl_global_active_slot}..."
	ab_set_active_slot_is_successful false
	ab_update_active_slot_boot_count increase
	ab_set_save_env abootctrl_global_current_slot $abootctrl_global_active_slot
}

if [ "$ab_enable" = "true" ]; then
	ab_init
else
	set boot_partition=$partition_boot
	export boot_partition
	set recovery_partition=$partition_recovery
	export recovery_partition
	set vendor_boot_partition=$partition_vendor_boot
	export vendor_boot_partition
fi

function set_android_default {
	save_setting grub_android_default "$1"
}

load_env -f ${partition_persist}/grubenv android_theme grub_android_default grub_timeout
if [ ! "$grub_android_default" ]; then
	set grub_android_default=0
fi
export android_theme grub_android_default grub_timeout

# Note: Command `loadstring` is not available in upstream grub source code
loadstring $partition_misc android_misc 20 0
if [ "$android_misc" = "boot-recovery" ]; then
	# Boot to recovery
	set default=1
	set timeout=$grub_timeout
elif [ "$android_misc" = "bootonce-bootloader" ]; then
	# Do nothing
	true
else
	# Normal boot
	set default="advancedoptions-virtio_common>android-virtio_common"
	set timeout=0
fi

# For temporary testing
#android_theme=1
#set default=0
#set timeout=30

set gfxmode=1024x768
if [ "$grub_platform" = "efi" ]; then
	set grub_gfx_module="efi_gop"
elif [ "$grub_platform" = "pc" ]; then
	set grub_gfx_module="vbe"
fi
if insmod $grub_gfx_module; then
	terminal_output gfxterm
	loadfont /boot/grub/fonts/unicode.pf2

	insmod gfxmenu
	insmod jpeg
	insmod png
	insmod tga

	if [ "$ab_init_failed" != "true" ]; then
		if [ -f /boot/grub/themes/lineage/theme.cfg ]; then
			source /boot/grub/themes/lineage/theme.cfg

			set export_theme="$theme"
			export export_theme
		fi
	fi
fi

if [ ! "$export_theme" ]; then
	if [ "$ab_init_failed" = "true" ]; then
		# Red
		background_color 255,0,0
		set timeout='-1'
	elif [ "$android_theme" = "1" ]; then
		# Light
		background_color 255,255,255
		set color_normal=black/white
	elif [ "$android_theme" = "2" ]; then
		# Dark
		background_color 20,20,20
	fi
fi

source /.grub_kernel_version.cfg

if [ "lineage" != "lineage" ]; then
	set kernel_version_display_mainmenu=" ${kernel_version_display}"
	export kernel_version_display_mainmenu
fi

function boot_android {
	pre_boot_hook
	echo 'Loading parameters...'
	source ${boot_partition}/boot-info.cfg
	source ${vendor_boot_partition}/vendor_boot-info.cfg
	source ${vendor_boot_partition}/vendor_boot-ramdisk-fragment-info.cfg
	set android_boot_img_cmdline="console=ttynull stack_depot_disable=on cgroup_disable=pressure kasan.stacktrace=off kvm-arm.mode=protected bootconfig ioremap_guard log_buf_len=4M loop.max_part=7 printk.devkmsg=on rw vt.global_cursor_default=0 androidboot.fstab_suffix=virtio.gsi.vdc androidboot.slot_suffix=_a androidboot.boot_devices=any androidboot.init_fatal_reboot_target=recovery androidboot.hypervisor.version=1 androidboot.hypervisor.vm.supported=1 androidboot.hypervisor.protected_vm.supported=0"
	set android_vendor_boot_img_cmdline="console=ttyAMA0"
	echo 'Loading kernel...'
	linux ${boot_partition}/kernel $android_boot_img_cmdline $android_vendor_boot_img_cmdline $kernel_cmdline_dynamic $@ 
	echo 'Loading ramdisk...'
	eval initrd ${boot_partition}/ramdisk.img ${android_vendor_boot_img_ramdisk_fragment_platform_list} ${android_vendor_boot_img_ramdisk_fragment_dlkm_list} ${vendor_boot_partition}/bootconfig.bin
}

function boot_android_recovery {
	pre_boot_hook recovery
	echo 'Loading parameters...'
	source ${boot_partition}/boot-info.cfg
	source ${vendor_boot_partition}/vendor_boot-info.cfg
	source ${vendor_boot_partition}/vendor_boot-ramdisk-fragment-info.cfg
	set android_boot_img_cmdline="console=ttynull stack_depot_disable=on cgroup_disable=pressure kasan.stacktrace=off kvm-arm.mode=protected bootconfig ioremap_guard log_buf_len=4M loop.max_part=7 printk.devkmsg=on rw vt.global_cursor_default=0 androidboot.fstab_suffix=virtio.gsi.vdc androidboot.slot_suffix=_a androidboot.boot_devices=any androidboot.init_fatal_reboot_target=recovery androidboot.hypervisor.version=1 androidboot.hypervisor.vm.supported=1 androidboot.hypervisor.protected_vm.supported=0"
	set android_vendor_boot_img_cmdline="console=ttyAMA0"
	echo 'Loading kernel...'
	linux ${recovery_partition}/kernel $android_boot_img_cmdline $android_vendor_boot_img_cmdline $kernel_cmdline_dynamic $@ 
	echo 'Loading recovery ramdisk...'
	eval initrd ${recovery_partition}/ramdisk.img ${android_vendor_boot_img_ramdisk_fragment_platform_list} ${android_vendor_boot_img_ramdisk_fragment_recovery_list} ${android_vendor_boot_img_ramdisk_fragment_dlkm_list} ${vendor_boot_partition}/bootconfig.bin
}

menuentry "LineageOS 22.2${kernel_version_display_mainmenu}" --class android --id android {
	set_android_default android
	boot_android
}

menuentry "LineageOS 22.2${kernel_version_display_mainmenu} Recovery" --class tools {
	boot_android_recovery
}

function print_error_non_ab {
	echo "Non-A/B does not support boot control"
	sleep 1
}

submenu "Settings" --class application-cog {
	if [ "$export_theme" ]; then
		set theme="$export_theme"
		export theme
	fi

	submenu "A/B boot control" --class ab-testing {
		menuentry "Show details" --class text {
			if [ "$ab_enable" = "true" ]; then
				if [ "$ab_init_failed" != true ]; then
					echo "Active slot: $abootctrl_global_active_slot"
					echo "Last booted slot: $abootctrl_global_current_slot"
					echo "Snapshot merge status: $abootctrl_global_snapshot_merge_status"
					for slot in $ab_slots; do
						var_name=abootctrl_slot_${slot}_boot_count
						eval var_value=\\$$var_name
						echo "Slot ${slot} - boot retry count: ${var_value}"
						var_name=abootctrl_slot_${slot}_is_bootable
						eval var_value=\\$$var_name
						echo "Slot ${slot} - is bootable: ${var_value}"
						var_name=abootctrl_slot_${slot}_is_successful
						eval var_value=\\$$var_name
						echo "Slot ${slot} - is successful: ${var_value}"
					done
					echo
					echo "Settings:"
					echo "- Disable automatic slot switching: $abootctrl_global_no_auto_slot_switch"
				else
					echo "A/B initialization has failed. Will boot slot ${abootctrl_global_active_slot} as fallback."
				fi
				sleep 3
			else
				print_error_non_ab
			fi
		}
		menuentry "Disable automatic slot switching" --class close-circle {
			if [ "$ab_enable" = "true" ]; then
				ab_set_save_env abootctrl_global_no_auto_slot_switch true
			else
				print_error_non_ab
			fi
		}
		menuentry "Enable automatic slot switching" --class check-circle {
			if [ "$ab_enable" = "true" ]; then
				ab_set_save_env abootctrl_global_no_auto_slot_switch false
			else
				print_error_non_ab
			fi
		}
		menuentry "Switch active slot" --class swap-horizontal-circle {
			if [ "$ab_enable" = "true" ]; then
				ab_switch_active_slot manual
				reboot # because it wouldn't get applied to the current boot
			else
				print_error_non_ab
			fi
		}
	}
	submenu "Boot animation" --class animation {
		menuentry "Show current setting" --class text {
			load_env -f ${partition_persist}/grubenv android_nobootanim
			if [ "$android_nobootanim" = "1" ]; then
				echo "Boot animation is disabled"
			else
				echo "Boot animation is enabled"
			fi
			sleep 1
		}
		menuentry "Enable boot animation" --class check-circle {
			save_setting android_nobootanim 0
		}
		menuentry "Disable boot animation" --class close-circle {
			save_setting android_nobootanim 1
		}
	}
	submenu "Display resolution" --class monitor-screenshot {
		menuentry "Show current setting" --class text {
			load_env -f ${partition_persist}/grubenv kms_display_resolution
			if [ "$kms_display_resolution" ]; then
				echo "Currently set display resolution is: $kms_display_resolution"
			else
				echo "No display resolution is set"
			fi
			sleep 1
		}
		menuentry "Unset (use default)" {
			save_setting kms_display_resolution
		}
		menuentry "360x640 (9:16)" {
			save_setting kms_display_resolution 360x640
		}
		menuentry "576x1024 (9:16)" {
			save_setting kms_display_resolution 576x1024
		}
		menuentry "720x1280 (9:16)" {
			save_setting kms_display_resolution 720x1280
		}
		menuentry "640x480 (4:3)" {
			save_setting kms_display_resolution 640x480
		}
		menuentry "800x600 (4:3)" {
			save_setting kms_display_resolution 800x600
		}
		menuentry "1024x768 (4:3)" {
			save_setting kms_display_resolution 1024x768
		}
		menuentry "1280x1024 (4:3)" {
			save_setting kms_display_resolution 1280x1024
		}
		menuentry "1536x1152 (4:3)" {
			save_setting kms_display_resolution 1536x1152
		}
		menuentry "2048x1536 (4:3)" {
			save_setting kms_display_resolution 2048x1536
		}
		menuentry "640x360 (16:9)" {
			save_setting kms_display_resolution 640x360
		}
		menuentry "1024x576 (16:9)" {
			save_setting kms_display_resolution 1024x576
		}
		menuentry "1280x720 (16:9)" {
			save_setting kms_display_resolution 1280x720
		}
		menuentry "1360x768 (16:9)" {
			save_setting kms_display_resolution 1360x768
		}
		menuentry "1600x900 (16:9)" {
			save_setting kms_display_resolution 1600x900
		}
		menuentry "1920x1080 (16:9)" {
			save_setting kms_display_resolution 1920x1080
		}
	}
	submenu "Insecure ADB" --class wrench {
		menuentry "Show current setting" --class text {
			load_env -f ${partition_persist}/grubenv android_insecure_adb
			if [ "$android_insecure_adb" = "1" ]; then
				echo "Insecure ADB is enabled"
			else
				echo "Insecure ADB is disabled"
			fi
			sleep 1
		}
		menuentry "Disable insecure ADB" --class close-circle {
			save_setting android_insecure_adb 0
		}
		menuentry "Enable insecure ADB" --class check-circle {
			save_setting android_insecure_adb 1
		}
	}
	submenu "Low performance optimizations" --class speedometer {
		menuentry "Show current setting" --class text {
			load_env -f ${partition_persist}/grubenv android_low_perf
			if [ "$android_low_perf" = "1" ]; then
				echo "Low performance optimizations is enabled"
			else
				echo "Low performance optimizations is disabled"
			fi
			sleep 1
		}
		menuentry "Disable low performance optimizations" --class close-circle {
			save_setting android_low_perf 0
		}
		menuentry "Enable low performance optimizations" --class check-circle {
			save_setting android_low_perf 1
		}
	}
	submenu "Mitigations" --class security {
		menuentry "Show current setting" --class text {
			load_env -f ${partition_persist}/grubenv mitigations
			if [ "$mitigations" = "off" ]; then
				echo "Mitigations is disabled"
			else
				echo "Mitigations is enabled"
			fi
			sleep 1
		}
		menuentry "Disable mitigations" --class shield-alert {
			save_setting mitigations off
		}
		menuentry "Enable mitigations" --class shield {
			save_setting mitigations auto
		}
	}
	submenu "Quiet boot" --class text-short {
		menuentry "Show current setting" --class text {
			load_env -f ${partition_persist}/grubenv quiet
			if [ "$quiet" = "1" ]; then
				echo "Quiet boot is enabled"
			else
				echo "Quiet boot is disabled"
			fi
			sleep 1
		}
		menuentry "Disable quiet boot" --class close-circle {
			save_setting quiet 0
		}
		menuentry "Enable quiet boot" --class check-circle {
			save_setting quiet 1
		}
	}
	submenu "Screen density" --class monitor-dashboard {
		menuentry "Show current setting" --class text {
			load_env -f ${partition_persist}/grubenv android_lcd_density
			if [ "$android_lcd_density" ]; then
				echo "Currently set screen density is: $android_lcd_density"
			else
				echo "No screen density is set"
			fi
			sleep 1
		}
		menuentry "Unset (use default)" {
			save_setting android_lcd_density
		}
		menuentry "MDPI (160 DPI)" {
			save_setting android_lcd_density 160
		}
		menuentry "HDPI (240 DPI)" {
			save_setting android_lcd_density 240
		}
		menuentry "XHDPI (320 DPI)" {
			save_setting android_lcd_density 320
		}
		menuentry "XXHDPI (480 DPI)" {
			save_setting android_lcd_density 480
		}
		menuentry "XXXHDPI (640 DPI)" {
			save_setting android_lcd_density 640
		}
	}
	submenu "SELinux" --class security {
		menuentry "Show current setting" --class text {
			load_env -f ${partition_persist}/grubenv android_selinux_permissive
			if [ "$android_selinux_permissive" = "1" ]; then
				echo "SELinux is in permissive mode"
			else
				echo "SELinux is in enforcing mode"
			fi
			sleep 1
		}
		menuentry "Enforcing" --class shield {
			save_setting android_selinux_permissive 0
		}
		menuentry "Permissive" --class shield-alert {
			save_setting android_selinux_permissive 1
		}
	}
	submenu "Wi-Fi implementation" --class wifi-cog {
		menuentry "Show current setting" --class text {
			load_env -f ${partition_persist}/grubenv android_wifi_impl
			if [ "$android_wifi_impl" ]; then
				echo "Currently set Wi-Fi implementation is: $android_wifi_impl"
			else
				echo "No Wi-Fi implementation is set"
			fi
			sleep 1
		}
		menuentry "No Wi-Fi implementation (Use Ethernet)" --class ethernet {
			save_setting android_wifi_impl
		}
		menuentry "VirtWifi using eth0 interface" --class wifi-arrow-up-down {
			save_setting android_wifi_impl virt_wifi
		}
	}
	submenu "GRUB: timeout" --class clock-edit {
		menuentry "Show current setting" --class text {
			load_env -f ${partition_persist}/grubenv grub_timeout
			if [ "$grub_timeout" ]; then
				echo "Currently set timeout is: $grub_timeout"
			else
				echo "No timeout is set"
			fi
			sleep 1
		}
		menuentry "Wait infinitely (-1)" {
			save_setting grub_timeout -1
		}
		menuentry "3 seconds" {
			save_setting grub_timeout 3
		}
		menuentry "5 seconds" {
			save_setting grub_timeout 5
		}
		menuentry "10 seconds" {
			save_setting grub_timeout 10
		}
		menuentry "30 seconds" {
			save_setting grub_timeout 30
		}
	}
}
insmod serial

submenu 'Advanced options' --class format-list-bulleted --id advancedoptions-virtio_common {
#SUPPORT_MESA_SWRAST#	menuentry "LineageOS 22.2 ${kernel_version_display} (Headless mode, Mesa swrast graphics)" --id android-virtio_common-headless_mesaswrast {
#SUPPORT_MESA_SWRAST#		set_android_default "advancedoptions-virtio_common>android-virtio_common-headless_mesaswrast"
#SUPPORT_MESA_SWRAST#		boot_android virtio_gpu.modeset=0 androidboot.gpu=vkms androidboot.graphics=mesa_swrast androidboot.insecure_adb=1
#SUPPORT_MESA_SWRAST#	}

	menuentry "LineageOS 22.2 ${kernel_version_display} (Single-disk, /system from super logical)" --id android-virtio_common {
		set_android_default "advancedoptions-virtio_common>android-virtio_common"
		boot_android androidboot.selinux=enforcing enforcing=1 androidboot.hardware=gs101 androidboot.bootloader=slider-15.3-13239612 androidboot.hardware.color=RED androidboot.hardware.coo=CN androidboot.hardware.ddr=8GiB,Samsung,LPDDR5,0108 androidboot.hardware.devcfg=G950-07197-01 androidboot.hardware.pcbcfg=G650-07839-05 androidboot.hardware.platform=gs101 androidboot.hardware.revision=MP1.0 androidboot.hardware.ufs=128GB,SKHynix androidboot.ddr_info=Samsung androidboot.ddr_size=8GiB androidboot.revision=MP1.0 androidboot.primary_panel_drv=panel-samsung-s6e3fc3 androidboot.hw.soc.id=09b64fbc2110 androidboot.hw.soc.nonsec-ar=6 androidboot.hw.soc.rev=3 androidboot.hw.soc.sec-ar=3 androidboot.avb_version=1.3 androidboot.vbmeta.avb_version=1.3 androidboot.vbmeta.device=PARTUUID=3928c12b-4a24-452e-92f0-b15030a38aeb androidboot.vbmeta.device_state=locked androidboot.vbmeta.hash_alg=sha256 androidboot.vbmeta.size=16768 androidboot.vbmeta.digest=fe049d35e450d9faf4b26788864f7b5fd5b857276d5271b067ef0e0268acff6f androidboot.verifiedbootstate=green androidboot.flash.locked=1 androidboot.secure_boot=PRODUCTION androidboot.veritymode=enforcing androidboot.veritymode.managed=yes androidboot.force_normal_boot=1 androidboot.load_modules_parallel=true androidboot.hypervisor.version=kvm.arm-nvhe androidboot.serialconsole=0 androidboot.dtbo_idx=0 androidboot.dpm_state=0x0000 androidboot.debug_level=0x4948 androidboot.cdt_hwid=0x00020306000100000100000000000000
	}

	menuentry "LineageOS 22.2 ${kernel_version_display} (Headless mode, Swiftshader graphics)" --id android-virtio_common-headless_swiftshader {
		set_android_default "advancedoptions-virtio_common>android-virtio_common-headless_swiftshader"
		boot_android virtio_gpu.modeset=0 androidboot.gpu=vkms androidboot.graphics=swiftshader androidboot.insecure_adb=1
	}

#SUPPORT_MESA_SWRAST#	menuentry "LineageOS 22.2 ${kernel_version_display} (Mesa swrast graphics)" --id android-virtio_common-mesaswrast {
#SUPPORT_MESA_SWRAST#		set_android_default "advancedoptions-virtio_common>android-virtio_common-mesaswrast"
#SUPPORT_MESA_SWRAST#		boot_android androidboot.graphics=mesa_swrast
#SUPPORT_MESA_SWRAST#	}

	menuentry "Boot GSI from /dev/block/sda with LineageOS 22.2 ${kernel_version_display}" --id android-virtio_common-gsifromsda {
		set_android_default "advancedoptions-virtio_common>android-virtio_common-gsifromsda"
		boot_android androidboot.fstab_suffix=virtio.gsi.sda
	}

	menuentry "Boot GSI from /dev/block/vdc with LineageOS 22.2 ${kernel_version_display}" --id android-virtio_common-gsifromvdc {
		set_android_default "advancedoptions-virtio_common>android-virtio_common-gsifromvdc"
		boot_android androidboot.fstab_suffix=virtio.gsi.vdc androidboot.selinux=enforcing enforcing=1 androidboot.hardware=gs101 androidboot.bootloader=slider-15.3-13239612 androidboot.hardware.color=RED androidboot.hardware.coo=CN androidboot.hardware.ddr=8GiB,Samsung,LPDDR5,0108 androidboot.hardware.devcfg=G950-07197-01 androidboot.hardware.pcbcfg=G650-07839-05 androidboot.hardware.platform=gs101 androidboot.hardware.revision=MP1.0 androidboot.hardware.ufs=128GB,SKHynix androidboot.ddr_info=Samsung androidboot.ddr_size=8GiB androidboot.revision=MP1.0 androidboot.primary_panel_drv=panel-samsung-s6e3fc3 androidboot.hw.soc.id=09b64fbc2110 androidboot.hw.soc.nonsec-ar=6 androidboot.hw.soc.rev=3 androidboot.hw.soc.sec-ar=3 androidboot.avb_version=1.3 androidboot.vbmeta.avb_version=1.3 androidboot.vbmeta.device=PARTUUID=3928c12b-4a24-452e-92f0-b15030a38aeb androidboot.vbmeta.device_state=locked androidboot.vbmeta.hash_alg=sha256 androidboot.vbmeta.size=16768 androidboot.vbmeta.digest=fe049d35e450d9faf4b26788864f7b5fd5b857276d5271b067ef0e0268acff6f androidboot.verifiedbootstate=green androidboot.flash.locked=1 androidboot.secure_boot=PRODUCTION androidboot.veritymode=enforcing androidboot.veritymode.managed=yes androidboot.force_normal_boot=1 androidboot.load_modules_parallel=true androidboot.hypervisor.version=kvm.arm-nvhe androidboot.serialconsole=0 androidboot.dtbo_idx=0 androidboot.dpm_state=0x0000 androidboot.debug_level=0x4948 androidboot.cdt_hwid=0x00020306000100000100000000000000
	}
}

serial
terminal_output --append serial
"""

# super 物理分区放大到 8GiB（oriole-16 / Android16：官方 product 3.8GiB，6GiB 不够；
# 叠加 vendor 1.5GiB 预留后共需约 6.95GiB）
SUPER_SIZE = 8 * 1024 * 1024 * 1024

# data.qcow2 虚拟大小：与基线 -baseline/data.qcow2 一致（110 GiB 稀疏种子）
DATA_VIRT_BYTES = 118398996480

SECTOR = 512
MIB = 1024 * 1024

# GPT 分区类型 GUID（原始字节，GPT 条目前 16 字节）
GUID_ESP = bytes.fromhex("28732ac11ff8d211ba4b00a0c93ec93b")
GUID_ANDROID_SYS = bytes.fromhex("af3dc60f838472478e793d69d8477de4")
# BIOS 分区（Intel 混合 GPT 引导代码区，基线与固件 BDS 按此解析）
GUID_BIOS_BOOT = bytes.fromhex("4861682149646f6e744e656564454649")

# 生成 super.img 的逻辑分区（name -> image）
# 逻辑分区名带 _a 槽位后缀（与官方 super、lineage 底包一致）：fstab.gs101 的
# logical 行都带 slotselect，init 按 ro.boot.slot_suffix(_a) 拼出 <name>_a 解析，
# 无后缀则解析不到分区（note13/popsicle 同款）。增量补丁同样用带后缀名：
#   python3 build/scripts/pack-roms.py --patch vendor_a=out/dist/rom/vendor.img
SUPER_PARTITIONS = [
    ("system_dlkm_a", os.path.join(ROMS_DIR, "system_other.img")),
    ("vendor_dlkm_a", os.path.join(ROM_DIST, "vendor_dlkm.img")),
    ("product_a", os.path.join(ROMS_DIR, "product.img")),
    # system_a 用官方固件件（verbatim）：本机型尚未移植 system 重打包阶段
    # （build/system/build 不存在），而 /vendor/{etc,bin,lib,lib64,apex} 与
    # /vendor/build.prop 已由内核重定向到 patch.img（lineage virtio vendor 内容），
    # system 侧不需要改动即可配平（同 popsicle-16 路线）。
    ("system_a", os.path.join(ROMS_DIR, "system.img")),
    ("system_ext_a", os.path.join(ROMS_DIR, "system_ext.img")),
    ("odm_a", os.path.join(LINEAGE_DIR, "super", "odm_a.img")),
    ("vendor_a", os.path.join(ROM_DIST, "vendor.img")),
]

# 组装 vda.img 的物理分区（name -> image）
# 分区顺序与基线盘对齐（recovery 后紧跟 firmware），确保固件 BDS 能按布局解析引导。
VDA_PARTITIONS = [
    ("EFI", os.path.join(LINEAGE_DIR, "EFI.img")),
    ("super", os.path.join(ROM_DIST, "super.img")),
    ("misc", os.path.join(LINEAGE_DIR, "misc.img")),
    ("metadata", os.path.join(LINEAGE_DIR, "metadata.img")),
    ("cache", os.path.join(LINEAGE_DIR, "cache.img")),
    # boot / vendor_boot 用槽位命名（_a）：fstab.gs101 沿用真机（A/B）的
    # `by-name/boot ... first_stage_mount,nofail,slotselect` 两行，init 会按
    # ro.boot.slot_suffix 解析成 by-name/boot_a。GPT 标签若是裸名，一阶段
    # BlockDevInitializer 等 10s 后直接 fatal（nofail 只跳过挂载，不跳过等设备）。
    # GRUB 按 gpt 序号（gpt6/gpt11）引用，与标签无关，改名不影响引导链。
    ("boot_a", os.path.join(ROM_DIST, "boot.img")),
    ("recovery", os.path.join(LINEAGE_DIR, "recovery.img")),
    ("firmware", os.path.join(LINEAGE_DIR, "firmware.img")),
    ("persist", os.path.join(LINEAGE_DIR, "persist.img")),
    ("BIOS", os.path.join(LINEAGE_DIR, "BIOS.img")),
    ("vendor_boot_a", os.path.join(ROM_DIST, "vendor_boot.img")),
    # frp（vda12）追加在末尾：保持 vda1..vda11 编号与 init.gs101.rc 的 by-name 软链、
    # GRUB 的 gpt6/gpt11 引用不变。PersistentDataBlockService 按
    # ro.frp.pst=/dev/block/by-name/frp 打开该分区，缺它则 pdb init 超时 →
    # "Failed to boot service ...PersistentDataBlockService" → system_server FATAL
    # 反复重启（实例 9 实测）。init 依 GPT 标签自动建 by-name/frp
    # （androidboot.boot_devices=any）；节点标签 frp_block_device（平台已给
    # system_server 授权）与 0660 root:system 由 vendor 的 file_contexts/ueventd.rc 提供。
    ("frp", os.path.join(LINEAGE_DIR, "frp.img")),
]

# 逻辑分区（super 内）预留容量：给频繁迭代的分区分配比镜像更大的 extent，
# 使后续「只改此分区」时能用 dd 原地覆盖 vda.img（新镜像 ≤ 容量即可），
# 无需重跑 build_super_image + build_vda。默认给 vendor 预留 1.5GiB。
# 增大预留需重新全量打包一次（--force）；之后所有 ≤ 预留的 vendor 改动都走增量。
SUPER_RESERVE = {
    "vendor_a": 1536 * MIB,
}


def _qemu_img():
    """返回本项目编译出的 qemu-img 二进制。"""
    goos = "darwin" if sys.platform.startswith("darwin") else "linux"
    path = os.path.join(OUT_ROOT, "qemu", "dist", goos, "bin", "qemu-img")
    if os.path.isfile(path):
        return path
    return shutil.which("qemu-img")


def _mtools(tool):
    """定位 mtools 命令（mcopy/mtype 等），用于就地修改 FAT 格式的 EFI 分区镜像。"""
    if shutil.which(tool):
        return shutil.which(tool)
    for pre in ("/opt/homebrew/bin", "/usr/local/bin", "/usr/bin"):
        cand = os.path.join(pre, tool)
        if os.path.isfile(cand):
            return cand
    raise RuntimeError(f"缺少 mtools 命令: {tool}（请先安装 mtools）")


def prepare_efi_partition():
    """把 LINEAGE EFI.img 复制到工作文件，并将其 /boot/grub/grub.cfg 覆写为可引导定制版。

    基线盘 ESP 与 lineage 原始 EFI.img 的 GRUB 二进制/模块/标记文件完全一致，
    唯一实质差异就是 grub.cfg。这里覆盖成基线版，让 vda 的 GRUB 与基线盘行为一致，
    从而正确完成 ESP 定位并启动内核。返回工作镜像路径供 vda 组装使用。
    """
    src = os.path.join(LINEAGE_DIR, "EFI.img")
    if not os.path.isfile(src):
        raise RuntimeError(f"缺少 EFI 源镜像: {src}")
    work = os.path.join(IMAGE_DIR, "efi.img")
    shutil.copy2(src, work)
    mcopy = _mtools("mcopy")
    grub_tmp = os.path.join(IMAGE_DIR, "grub.cfg")
    with open(grub_tmp, "wb") as f:
        f.write(GRUB_CFG_CONTENT)
    try:
        proc = subprocess.run(
            [mcopy, "-o", "-i", work, grub_tmp, "::/boot/grub/grub.cfg"],
            capture_output=True, text=True)
    finally:
        os.remove(grub_tmp)
    if proc.returncode != 0:
        raise RuntimeError(f"覆写 ESP grub.cfg 失败: {proc.stderr.strip()}")
    print(f"[ok] EFI 分区 grub.cfg 已定制: {work}（内联基线 grub 常量）")
    return work


# ---------- GPT / vda.img ----------

def _gpt_crc32(data):
    return zlib.crc32(data) & 0xFFFFFFFF


def _part_guid(name):
    return uuid.uuid5(uuid.NAMESPACE_DNS, name).bytes


def _gpt_entry(type_guid, part_guid, first, last, name, attrs=0):
    ent = bytearray(128)
    ent[0:16] = type_guid
    ent[16:32] = part_guid
    struct.pack_into("<QQ", ent, 32, first, last)
    struct.pack_into("<Q", ent, 48, attrs)
    ent[56:56 + 72] = name.encode("utf-16-le").ljust(72, b"\x00")
    return bytes(ent)


def build_vda(partitions, out_path):
    """组装 GPT 磁盘（vda.img）。partitions: list[(name, image_path)]，1MiB 对齐。"""
    os.makedirs(os.path.dirname(out_path), exist_ok=True)

    # 计算各分区 LBA
    plan = []  # (name, image, first_sector, num_sectors)
    cursor = 2048
    for name, image in partitions:
        size = os.path.getsize(image)
        num = (size + SECTOR - 1) // SECTOR
        plan.append((name, image, cursor, num))
        cursor = _align_up(cursor + num, MIB // SECTOR)
    last_usable = plan[-1][2] + plan[-1][3] - 1
    last_disk_lba = last_usable + 33        # + 备份 GPT（32 项分区表 + 1 头）
    disk_sectors = last_disk_lba + 1
    disk_size = disk_sectors * SECTOR

    NUM_ENTRIES = 128
    ENTRIES_LBA = 2
    entries = bytearray(NUM_ENTRIES * 128)
    for i, (name, image, first, num) in enumerate(plan):
        if name == "EFI":
            type_guid = GUID_ESP
        elif name == "BIOS":
            type_guid = GUID_BIOS_BOOT
        else:
            type_guid = GUID_ANDROID_SYS
        entries[i * 128:(i + 1) * 128] = _gpt_entry(
            type_guid, _part_guid(name), first, first + num - 1, name)

    array_crc = _gpt_crc32(entries)

    # 主 GPT 头（LBA1）
    hdr = bytearray(92)
    hdr[0:8] = b"EFI PART"
    struct.pack_into("<I", hdr, 8, 0x00010000)   # revision
    struct.pack_into("<I", hdr, 12, 92)          # header_size
    struct.pack_into("<Q", hdr, 24, 1)           # current_lba
    struct.pack_into("<Q", hdr, 32, last_disk_lba)  # backup_lba
    struct.pack_into("<Q", hdr, 40, 2048)        # first_usable_lba
    struct.pack_into("<Q", hdr, 48, last_usable) # last_usable_lba
    hdr[56:72] = uuid.UUID("02000000-0000-0080-8000-000000000000").bytes  # disk_guid
    struct.pack_into("<Q", hdr, 72, ENTRIES_LBA) # partition_entries_lba
    struct.pack_into("<I", hdr, 80, NUM_ENTRIES) # num_partition_entries
    struct.pack_into("<I", hdr, 84, 128)         # entry_size
    struct.pack_into("<I", hdr, 88, array_crc)   # array crc
    hdr[16:20] = bytes(4)  # 头 crc 校验字段先置 0
    hdr[16:20] = struct.pack("<I", _gpt_crc32(bytes(hdr)))

    # 备份 GPT 头（拷贝自主头，需切换 current/backup_lba 并重算 crc）
    bk = bytearray(hdr)
    struct.pack_into("<Q", bk, 24, last_disk_lba)  # current_lba（备份）
    struct.pack_into("<Q", bk, 32, 1)              # backup_lba（备份备份=主头）
    struct.pack_into("<I", bk, 88, array_crc)
    bk[16:20] = bytes(4)  # 先置 0 再算头 crc
    bk[16:20] = struct.pack("<I", _gpt_crc32(bytes(bk)))

    print(f"[vda] 组装 GPT 镜像: {len(plan)} 分区, 总大小 {disk_size} B (LBA 0..{disk_sectors - 1})")
    with open(out_path, "wb") as f:
        f.truncate(disk_size)
        # LBA0 标准 GPT 保护性 MBR：type 0xEE 位于 450、first_lba=1、size=整盘扇区数。
        # 若 size 为 0 或 type 位错（如 "<BHBI" 把 0xEE 写进 449），edk2 会把该盘判为
        # 不可引导 GPT，导致 Boot0001 "UEFI Misc Device" 加载失败、直接落到 UEFI Shell。
        f.seek(0)
        mbr = bytearray(512)
        struct.pack_into("<I", mbr, 440, 0x00000000)          # disk signature（0）
        struct.pack_into("<B", mbr, 446, 0x00)                # boot flag
        struct.pack_into("<BBB", mbr, 447, 0x00, 0x02, 0x00)  # CHS start
        struct.pack_into("<B", mbr, 450, 0xEE)                # protective type
        struct.pack_into("<BBB", mbr, 451, 0xFF, 0xFF, 0xFF)  # CHS end
        struct.pack_into("<I", mbr, 454, 1)                   # first LBA
        struct.pack_into("<I", mbr, 458, disk_sectors & 0xFFFFFFFF)  # protected size
        # 引导签名必须是内存序 55 AA（offset510=0x55, 511=0xAA）。勿用
        # struct.pack_into("<H", 510, 0x55AA)：小端会写成 AA 55，GRUB/EDK2 校验
        # 失败 → 判为不可引导 GPT → Boot0001 落 GRUB、marker search 失败。
        mbr[510] = 0x55
        mbr[511] = 0xAA
        f.write(bytes(mbr))
        # 主 GPT 头
        f.seek(SECTOR)
        f.write(bytes(hdr))
        # 分区条目
        f.seek(ENTRIES_LBA * SECTOR)
        f.write(bytes(entries))
        # 各分区数据
        for name, image, first, num in plan:
            f.seek(first * SECTOR)
            with open(image, "rb") as src:
                shutil.copyfileobj(src, f)
        # 备份分区条目 + 备份头
        f.seek((last_disk_lba - 32) * SECTOR)
        f.write(bytes(entries))
        f.seek(last_disk_lba * SECTOR)
        f.write(bytes(bk))
    print(f"[vda] 完成: {os.path.getsize(out_path)} B")
    return plan


def _align_up(v, a):
    return (v + a - 1) // a * a


# ---------- 增量补丁：只改单个分区，dd 写回 vda.img ----------
#
# 原理：GPT 头部 CRC 只覆盖分区表条目（名字/GUID/LBA），liblp 元数据 checksum
# 只覆盖元数据表——两者都不覆盖分区数据本身。因此只要新镜像 ≤ 已分配容量，
# 直接把数据写到对应字节偏移即可，无需重算任何校验和、无需重建 super/vda。
#
# 物理分区（super/boot/vendor_boot/...）：偏移取自 vda 的 GPT（first_lba×512），
#   容量 = (last_lba-first_lba+1)×512（GPT 分配的扇区，含 1MiB 对齐冗余）。
# 逻辑分区（vendor/system/product/...）：先在 GPT 里定位 super，再读 super 内
#   liblp 元数据定位 extent；容量 = extent 分配容量（构建时经 SUPER_RESERVE 预留）。


def _read_gpt_partitions(vda_path):
    """解析 vda.img 主 GPT，返回 dict[name -> (first_lba, last_lba)]。"""
    with open(vda_path, "rb") as f:
        f.seek(SECTOR)                      # 主 GPT 头在 LBA1
        hdr = f.read(92)
        if hdr[0:8] != b"EFI PART":
            raise ValueError(f"非 GPT 磁盘（缺 EFI PART 魔数）: {vda_path}")
        entries_lba = struct.unpack_from("<Q", hdr, 72)[0]
        num_entries = struct.unpack_from("<I", hdr, 80)[0]
        entry_size = struct.unpack_from("<I", hdr, 84)[0]
        f.seek(entries_lba * SECTOR)
        blob = f.read(num_entries * entry_size)
    out = {}
    for i in range(num_entries):
        ent = blob[i * entry_size:(i + 1) * entry_size]
        if ent[0:16] == bytes(16):          # 空类型 GUID = 未使用条目
            continue
        first, last = struct.unpack_from("<QQ", ent, 32)
        name = ent[56:128].decode("utf-16-le").split("\x00", 1)[0]
        if name:
            out[name] = (first, last)
    return out


def _locate_partition(vda_path, name):
    """定位分区在 vda.img 中的 (data_offset_bytes, capacity_bytes, kind)。

    kind ∈ {"physical","logical"}。找不到返回 None。
    """
    gpt = _read_gpt_partitions(vda_path)
    if name in gpt:
        first, last = gpt[name]
        return (first * SECTOR, (last - first + 1) * SECTOR, "physical")
    # 不是物理分区 → 尝试当逻辑分区（在 super 内）解析
    if "super" not in gpt:
        return None
    super_first, super_last = gpt["super"]
    super_base = super_first * SECTOR
    # 用一个只读“视图文件”让 pack_super 从 super_base 解析 liblp：
    # pack_super.locate_logical_partition 期望独立 super 文件，这里传入偏移视图。
    loc = _locate_logical_in_vda(vda_path, super_base, name)
    if loc is None:
        return None
    rel_off, capacity = loc
    return (super_base + rel_off, capacity, "logical")


def _locate_logical_in_vda(vda_path, super_base, name):
    """在 vda.img 内偏移 super_base 处的 super 里定位逻辑分区。

    复用 pack_super 的 liblp 解析常量，直接从 vda 读元数据（避免抽出 super 临时文件）。
    返回 (relative_data_offset, capacity_bytes) 或 None。
    """
    ps = pack_super
    meta_start = ps.RESERVED_BYTES + ps.GEOMETRY_SIZE * 2          # super 内偏移 12288
    with open(vda_path, "rb") as f:
        f.seek(super_base + meta_start)
        hdr = f.read(ps.LP_METADATA_HEADER_SIZE)
        po, pc, pe = struct.unpack_from("<III", hdr, ps.LP_METADATA_HEADER_PARTITIONS_OFF)
        eo, ec, ee = struct.unpack_from("<III", hdr, ps.LP_METADATA_HEADER_EXTENTS_OFF)
        tables_base = super_base + meta_start + ps.LP_METADATA_HEADER_SIZE
        # 读分区表
        f.seek(tables_base + po)
        part_blob = f.read(pc * pe)
        # 读 extent 表
        f.seek(tables_base + eo)
        ext_blob = f.read(ec * ee)
    extents = []
    for i in range(ec):
        ns = struct.unpack_from("<Q", ext_blob, i * ee)[0]
        td = struct.unpack_from("<Q", ext_blob, i * ee + 12)[0]
        extents.append((ns, td))
    for i in range(pc):
        base = i * pe
        pname = part_blob[base:base + 36].split(b"\x00", 1)[0].decode("ascii")
        _, first_extent, num_extents, _ = struct.unpack_from("<IIII", part_blob, base + 36)
        if pname == name:
            capacity = sum(extents[first_extent + j][0] for j in range(num_extents)) * SECTOR
            rel_off = extents[first_extent][1] * SECTOR      # target_data 扇区（super 内偏移）
            return (rel_off, capacity)
    return None


def list_partitions(vda_path):
    """打印 vda.img 里所有可增量补丁的分区（物理 + 逻辑）及容量/冗余。"""
    if not os.path.isfile(vda_path):
        print(f"[err] 找不到 vda.img: {vda_path}")
        return 1
    print(f"[list] {vda_path} ({os.path.getsize(vda_path)} B)")
    gpt = _read_gpt_partitions(vda_path)
    print("  物理分区（GPT）:")
    for name in sorted(gpt, key=lambda n: gpt[n][0]):
        first, last = gpt[name]
        cap = (last - first + 1) * SECTOR
        print(f"    {name:14s} offset={first * SECTOR:>12d} capacity={cap:>12d} B")
    if "super" in gpt:
        super_base = gpt["super"][0] * SECTOR
        # 逻辑分区名直接取 SUPER_PARTITIONS（单一数据源，避免与槽位命名漂移）
        print("  逻辑分区（super/liblp）:")
        for name, _img in SUPER_PARTITIONS:
            loc = _locate_logical_in_vda(vda_path, super_base, name)
            if loc:
                rel_off, cap = loc
                print(f"    {name:14s} offset={super_base + rel_off:>12d} capacity={cap:>12d} B")
            else:
                print(f"    {name:14s} 在 super 元数据里找不到")
    return 0


def patch_partition(vda_path, name, image_path, dry_run=False):
    """把 image_path 写入 vda.img 中 name 分区所在偏移（新镜像 ≤ 容量才允许）。"""
    if not os.path.isfile(vda_path):
        print(f"[err] 找不到 vda.img: {vda_path}")
        return 1
    if not os.path.isfile(image_path):
        print(f"[err] 找不到分区镜像: {image_path}")
        return 1
    loc = _locate_partition(vda_path, name)
    if loc is None:
        print(f"[err] vda.img 中找不到分区: {name}")
        return 1
    offset, capacity, kind = loc
    new_size = os.path.getsize(image_path)
    print(f"[patch] {name}（{kind}）: offset={offset} capacity={capacity} B "
          f"新镜像={new_size} B")
    if new_size > capacity:
        over = new_size - capacity
        print(f"[err] 新镜像超出已分配容量 {over} B。")
        if kind == "logical":
            print(f"      提高 SUPER_RESERVE['{name}'] 后重新全量打包（--force），"
                  f"或直接 --force 重建 vda。")
        else:
            print(f"      物理分区容量由 GPT 布局固定，需 --force 重建 vda。")
        return 1
    if dry_run:
        print(f"[..] (dry) 将 dd {image_path} -> {vda_path} @ {offset}（{new_size} B）")
        return 0
    # 原地覆盖：只写新镜像字节；尾部残留旧数据无害（ext4/erofs superblock 自带真实
    # 文件系统大小，dm-linear 仅映射固定扇区，内核不会读到 extent 尾部残留）。
    with open(vda_path, "r+b") as f:
        f.seek(offset)
        with open(image_path, "rb") as src:
            shutil.copyfileobj(src, f, length=8 * MIB)
        f.flush()
        os.fsync(f.fileno())
    print(f"[ok] 已增量写入 {name}: {new_size} B @ {offset}（未改 GPT/liblp 校验和）")
    return 0


# ---------- 源分区镜像保底修复 ----------

def _is_ext4(path):
    """判断镜像是否为有效 ext4（offset 1024 处 s_magic == 0xEF53）。"""
    with open(path, "rb") as f:
        b = f.read(2048)
    return struct.unpack_from("<H", b, 1024 + 56)[0] == 0xEF53


def _locate_mke2fs():
    """定位 Android SDK 自带 mke2fs（1.47+），缺失则退回 PATH。"""
    for cand in ("/Users/cuimingqiang/Library/Android/sdk/platform-tools/mke2fs",):
        if os.path.isfile(cand):
            return cand
    w = shutil.which("mke2fs")
    return w


# lineage 底包（UTM zip 的 vda.img）只导出 super/boot 链与 EFI/misc/metadata/firmware/
# persist/BIOS，cache 与 recovery 两个分区没有单独镜像；build_vda 按文件大小定容，
# 故用与底包 GPT 一致的 64 MiB 全零占位（fstab.gs101 不挂 /cache，recovery 不在
# GRUB 引导链里）。
PLACEHOLDER_CAPACITY = {"cache": 64 * MIB, "recovery": 64 * MIB, "frp": 1 * MIB}


def ensure_vda_source(name, work_dir, dry_run=False):
    """vda 源镜像缺失时按占位容量生成全零镜像；不可占位则返回 None。"""
    src = next(p for n, p in VDA_PARTITIONS if n == name)
    if os.path.isfile(src):
        return src
    cap = PLACEHOLDER_CAPACITY.get(name)
    if cap is None:
        return None
    os.makedirs(work_dir, exist_ok=True)
    out = os.path.join(work_dir, f"{name}.img")
    if dry_run:
        print(f"[..] (dry) 将生成 {name} 占位镜像（{cap} B 全零）-> {out}")
        return out
    with open(out, "wb") as f:
        f.truncate(cap)
    print(f"[ok] {name} 源缺失（{src}），生成 {cap} B 全零占位 -> {out}")
    return out


def ensure_metadata_ext4(work_dir, dry_run=False):
    """确保 metadata 源分区为有效 ext4。

    lineage 导出的 metadata.img 是空白镜像（非 ext4），first-stage 挂载 /metadata
    会报 'Invalid ext4 superblock'，导致 /metadata 挂为 Read-only，进而 keystore2/
    vold/zygote 连锁启动失败。这里若校验非法，则用 mke2fs 重生成有效 ext4 工作镜像。
    """
    src = next(p for n, p in VDA_PARTITIONS if n == "metadata")
    if _is_ext4(src):
        return src
    mke2fs = _locate_mke2fs()
    if not mke2fs:
        raise RuntimeError("缺少 mke2fs，无法格式化 metadata 分区（请安装 Android SDK platform-tools）")
    size = os.path.getsize(src)
    os.makedirs(work_dir, exist_ok=True)
    out = os.path.join(work_dir, "metadata.img")
    if dry_run:
        print(f"[..] (dry) 将格式化 metadata: {src} ({size} B, 非法 ext4) -> {out}")
        return out
    with open(out, "wb") as f:
        f.truncate(size)
    proc = subprocess.run(
        [mke2fs, "-t", "ext4", "-b", "4096", "-I", "256", "-m", "0", out],
        capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(f"mke2fs 格式化 metadata 失败: {proc.stderr.strip()}")
    if not _is_ext4(out):
        raise RuntimeError("metadata 格式化后仍非有效 ext4")
    print(f"[..] metadata 源非法，已重格式化为有效 ext4 ({size} B) -> {out}")
    return out


def _is_vfat(path):
    """判断镜像是否为有效 FAT 引导扇区（0x55AA 签名 + FilSysType 串）。"""
    with open(path, "rb") as f:
        b = f.read(512)
    if b[510:512] != b"\x55\xaa":
        return False
    # FAT12/16 的 FilSysType 在偏移 0x36，FAT32 在 0x52
    return b[54:59] in (b"FAT12", b"FAT16") or b[82:87] == b"FAT32"


def ensure_firmware_vfat(work_dir, dry_run=False):
    """确保 firmware 源分区为有效 vfat。

    lineage 底包的 firmware.img 是 128 MiB 全零（无任何 FAT 结构），fstab.gs101 的
    vfat 行挂它必然失败（VFS: Can't find a valid FAT filesystem），/vendor/firmware_mnt
    永远空挂。这里校验非 FAT 则在原容量上用 mformat 格式化成空 vfat 工作镜像。
    """
    src = next(p for n, p in VDA_PARTITIONS if n == "firmware")
    if _is_vfat(src):
        return src
    size = os.path.getsize(src)
    os.makedirs(work_dir, exist_ok=True)
    out = os.path.join(work_dir, "firmware.img")
    if dry_run:
        print(f"[..] (dry) 将格式化 firmware: {src} ({size} B, 非 FAT) -> {out}")
        return out
    with open(out, "wb") as f:
        f.truncate(size)
    proc = subprocess.run([_mtools("mformat"), "-m", "0xf8", "-i", out, "::"],
                          capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(f"mformat 格式化 firmware 失败: {proc.stderr.strip()}")
    if not _is_vfat(out):
        raise RuntimeError("firmware 格式化后仍非有效 FAT")
    print(f"[..] firmware 源非 FAT，已格式化为空 vfat ({size} B) -> {out}")
    return out


# ---------- data.qcow2 ----------

def make_data_qcow2(out_path, force, dry_run):
    """生成 110 GiB 空 qcow2 稀疏种子（与基线 data.qcow2 一致）。"""
    if os.path.isfile(out_path) and not force:
        print(f"[ok] data.qcow2 已就位（跳过）: {os.path.getsize(out_path)} B")
        return
    if dry_run:
        print(f"[..] (dry) 将生成 data.qcow2 ({DATA_VIRT_BYTES} B 稀疏)")
        return
    qemu_img = _qemu_img()
    if not qemu_img:
        print("[err] 未找到 qemu-img，无法生成 data.qcow2")
        sys.exit(1)
    cmd = [qemu_img, "create", "-f", "qcow2",
           "-o", "compat=1.1,cluster_size=65536",
           out_path, str(DATA_VIRT_BYTES)]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        print(f"[err] qemu-img 创建失败: {proc.stderr.strip()}")
        sys.exit(1)
    print(f"[ok] data.qcow2: {os.path.getsize(out_path)} B <== {cmd[0]}")


def pack(force=False, clean=False, dry_run=False):
    missing = [n for n, p in SUPER_PARTITIONS if not os.path.isfile(p)]
    if missing:
        print(f"[err] 缺少 super 逻辑分区镜像: {missing}")
        sys.exit(1)
    vda_src = {}
    for n, p in VDA_PARTITIONS:
        if n == "super":  # super.img 由本脚本生成
            continue
        got = ensure_vda_source(n, IMAGE_DIR, dry_run)
        if got is None:
            print(f"[err] 缺少 vda 源镜像: {n} ({p})")
            sys.exit(1)
        vda_src[n] = got

    if clean and not dry_run:
        if os.path.isdir(IMAGE_DIR):
            shutil.rmtree(IMAGE_DIR)
            print(f"[..] 已清空: {IMAGE_DIR}")
    os.makedirs(IMAGE_DIR, exist_ok=True)

    # 1) 生成 super.img
    super_img = os.path.join(ROM_DIST, "super.img")
    if force or not os.path.isfile(super_img) or dry_run:
        if dry_run:
            print("[..] (dry) 将生成 super.img:")
            for n, p in SUPER_PARTITIONS:
                print(f"      {n:14s} <- {p} ({os.path.getsize(p)} B)")
        else:
            pack_super.build_super_image(SUPER_PARTITIONS, SUPER_SIZE, super_img,
                                         reserve=SUPER_RESERVE)
    else:
        print(f"[ok] super.img 已就位（跳过）: {os.path.getsize(super_img)} B")

    # 2) 校验 super.img 可读回 7 分区
    if not dry_run:
        _, rows = pack_super.read_super_image(super_img)
        print(f"[chk] super.img 读回 {len(rows)} 个逻辑分区:")
        for name, size, data_sec in rows:
            print(f"      {name:14s} size={size} B data_sector={data_sec}")

    # 3) 组装 vda.img
    vda_img = os.path.join(IMAGE_DIR, "vda.img")
    # EFI 分区优先使用定制 grub.cfg 的工作镜像，否则退回 lineage 原始 EFI.img
    efi_part = prepare_efi_partition() if not dry_run else None
    efi_vec = [os.path.join(LINEAGE_DIR, "EFI.img")] if efi_part is None else [efi_part]
    # super 用本脚本刚生成的 ROM_DIST/super.img，其余源镜像经占位/校验解析
    parts = [efi_vec[0] if n == "EFI" else (p if n == "super" else vda_src[n])
             for n, p in VDA_PARTITIONS]
    # metadata 源若非法（非有效 ext4）则重格式化为有效 ext4（否则 /metadata 挂为
    # Read-only，keystore2/vold/zygote 连锁启动失败）
    meta_part = ensure_metadata_ext4(IMAGE_DIR, dry_run) if not dry_run else \
        [p for n, p in VDA_PARTITIONS if n == "metadata"][0]
    parts = [meta_part if n == "metadata" else p for n, p in zip([n for n, _ in VDA_PARTITIONS], parts)]
    # firmware 源非 FAT 则格式化（否则 /vendor/firmware_mnt 的 vfat 行挂载失败）
    fw_part = ensure_firmware_vfat(IMAGE_DIR, dry_run) if not dry_run else \
        [p for n, p in VDA_PARTITIONS if n == "firmware"][0]
    parts = [fw_part if n == "firmware" else p for n, p in zip([n for n, _ in VDA_PARTITIONS], parts)]
    vda_partitions = list(zip([n for n, _ in VDA_PARTITIONS], parts))
    # EFI grub.cfg 定制微波即重建 vda（efi.img 每次都会新生成）
    efi_newer = (not dry_run and efi_part is not None
                 and (not os.path.isfile(vda_img)
                      or os.path.getmtime(efi_part) > os.path.getmtime(vda_img)))
    if force or dry_run or efi_newer:
        if dry_run:
            print("[..] (dry) 将组装 vda.img (GPT):")
            for n, p in vda_partitions:
                sz = os.path.getsize(p) if os.path.isfile(p) else SUPER_SIZE
                print(f"      {n:14s} <- {p} ({sz} B)")
        else:
            build_vda(vda_partitions, vda_img)
    else:
        print(f"[ok] vda.img 已就位（跳过）: {os.path.getsize(vda_img)} B")

    # 4) 生成 data.qcow2
    make_data_qcow2(os.path.join(IMAGE_DIR, "data.qcow2"), force, dry_run)

    # 5) patch.img（vendor/vendor_dlkm 修补内容 + /mnt/vendor 骨架）落进运行盘集，
    #    daemon 检测到同名文件才挂 vdc（-gsi-image）；缺它则内核重定向
    #    /vendor/{etc,bin,lib,lib64,apex} -> /mnt/vendor/... 全部 ENOENT。
    if dry_run:
        print(f"[..] (dry) 将拷贝 patch.img -> {os.path.join(IMAGE_DIR, 'patch.img')}")
    elif os.path.isfile(PATCH_DIST):
        patch_target = os.path.join(IMAGE_DIR, "patch.img")
        shutil.copy2(PATCH_DIST, patch_target)
        print(f"[ok] patch.img -> {patch_target}"
              f"（{os.path.getsize(patch_target)} B）")
    else:
        print(f"[warn] 缺 patch.img: {PATCH_DIST}（先跑 patch 阶段），vdc 将不挂载")
    return 0


def main():
    ap = argparse.ArgumentParser(description="Pixel6-15 最终 ROM 打包（vda+data 组装）")
    ap.add_argument("--force", action="store_true", help="强制重建/覆盖")
    ap.add_argument("--clean", action="store_true", help="先清空目标目录")
    ap.add_argument("--dry-run", action="store_true", help="只打印将执行的动作")
    ap.add_argument("--patch", metavar="NAME=IMAGE", action="append", default=[],
                    help="增量补丁：把 IMAGE 直接 dd 写入已有 vda.img 的 NAME 分区，"
                         "不重建 super/vda（新镜像须 ≤ 已分配容量）。可多次指定。"
                         "例: --patch vendor_a=out/dist/rom/vendor.img")
    ap.add_argument("--list-partitions", action="store_true",
                    help="列出已有 vda.img 里所有可增量补丁的分区及容量/冗余")
    ap.add_argument("--vda", metavar="PATH", default=None,
                    help="指定 vda.img 路径（默认 <IMAGE_DIR>/vda.img），供 --patch/--list-partitions")
    args = ap.parse_args()

    vda_path = args.vda or os.path.join(IMAGE_DIR, "vda.img")

    if args.list_partitions:
        sys.exit(list_partitions(vda_path))

    if args.patch:
        rc = 0
        for spec in args.patch:
            if "=" not in spec:
                print(f"[err] --patch 需 NAME=IMAGE 格式: {spec}")
                rc = 1
                continue
            name, image = spec.split("=", 1)
            rc |= patch_partition(vda_path, name, image, dry_run=args.dry_run)
        sys.exit(rc)

    sys.exit(pack(force=args.force, clean=args.clean, dry_run=args.dry_run))


if __name__ == "__main__":
    main()