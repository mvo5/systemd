/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "varlink-io.systemd.Shutdown.h"

static SD_VARLINK_DEFINE_METHOD(PowerOff);
static SD_VARLINK_DEFINE_METHOD(Reboot);
static SD_VARLINK_DEFINE_METHOD(Halt);
static SD_VARLINK_DEFINE_METHOD(Kexec);
static SD_VARLINK_DEFINE_METHOD(SoftReboot);

static SD_VARLINK_DEFINE_METHOD(
                CanPowerOff,
                SD_VARLINK_DEFINE_OUTPUT(result, SD_VARLINK_STRING, 0));
static SD_VARLINK_DEFINE_METHOD(
                CanReboot,
                SD_VARLINK_DEFINE_OUTPUT(result, SD_VARLINK_STRING, 0));
static SD_VARLINK_DEFINE_METHOD(
                CanHalt,
                SD_VARLINK_DEFINE_OUTPUT(result, SD_VARLINK_STRING, 0));
static SD_VARLINK_DEFINE_ERROR(AlreadyInProgress);

SD_VARLINK_DEFINE_INTERFACE(
                io_systemd_Shutdown,
                "io.systemd.Shutdown",
                SD_VARLINK_INTERFACE_COMMENT("APIs for shutting down or rebooting the system."),
                SD_VARLINK_SYMBOL_COMMENT("Power off the system"),
                &vl_method_PowerOff,
                SD_VARLINK_SYMBOL_COMMENT("Reboot the system"),
                &vl_method_Reboot,
                SD_VARLINK_SYMBOL_COMMENT("Halt the system"),
                &vl_method_Halt,
                SD_VARLINK_SYMBOL_COMMENT("Reboot the system via kexec"),
                &vl_method_Kexec,
                SD_VARLINK_SYMBOL_COMMENT("Reboot userspace only"),
                &vl_method_SoftReboot,
                SD_VARLINK_SYMBOL_COMMENT("Check whether power-off is available and permitted. Returns yes, no, or challenge."),
                &vl_method_CanPowerOff,
                SD_VARLINK_SYMBOL_COMMENT("Check whether reboot is available and permitted"),
                &vl_method_CanReboot,
                SD_VARLINK_SYMBOL_COMMENT("Check whether halt is available and permitted"),
                &vl_method_CanHalt,
                SD_VARLINK_SYMBOL_COMMENT("Another shutdown or sleep operation is already in progress"),
                &vl_error_AlreadyInProgress);
